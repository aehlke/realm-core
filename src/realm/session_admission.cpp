#include <realm/session_admission.hpp>
#include <realm/group.hpp>
#include <realm/table.hpp>
#include <realm/util/sha_crypto.hpp>
#include <realm/util/file.hpp>
#include <cerrno>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <algorithm>
#include <set>

namespace realm {
SessionAdmissionError::SessionAdmissionError(Reason reason, std::string table)
    : std::runtime_error("Realm session admission refused (" + std::to_string(int(reason)) + ")"
                         + (table.empty() ? "" : ": " + table)), reason(reason), table(std::move(table)) {}

namespace {
bool valid_name(const std::string& name, bool empty = false)
{ return (empty || !name.empty()) && name.size() <= 256 && name.find('\0') == std::string::npos; }
void frame(std::string& output, const std::string& value)
{ output += std::to_string(value.size()) + ":" + value; }
}

std::vector<SessionAdmission::Table> SessionAdmission::validate(std::vector<Table> tables)
{
    using R = SessionAdmissionError::Reason;
    if (tables.empty() || tables.size() > 3) throw SessionAdmissionError(R::invalid_descriptor);
    std::set<std::string> names;
    for (auto& table : tables) {
        if (!valid_name(table.name) || !valid_name(table.primary_key, true) || table.kind > 2
            || table.columns.size() > 64 || !names.insert(table.name).second)
            throw SessionAdmissionError(R::invalid_descriptor);
        std::set<std::string> columns;
        bool has_primary = table.primary_key.empty();
        for (auto& column : table.columns) {
            if (!valid_name(column.name) || !valid_name(column.link_target, true) || !column.type.is_valid()
                || (column.collection != 0 && column.collection != 1 && column.collection != 2 && column.collection != 4)
                || (column.type == type_Link) != !column.link_target.empty()
                || !columns.insert(column.name).second) throw SessionAdmissionError(R::invalid_descriptor);
            if (column.name == table.primary_key) has_primary = true;
        }
        if (!has_primary) throw SessionAdmissionError(R::invalid_descriptor);
        std::sort(table.columns.begin(), table.columns.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    }
    std::sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return tables;
}

SessionAdmission::Identity SessionAdmission::fingerprint(const std::string& identifier, const std::vector<Table>& tables)
{
    if (!valid_name(identifier)) throw SessionAdmissionError(SessionAdmissionError::Reason::invalid_descriptor);
    std::string bytes;
    frame(bytes, "realm-empty-physical-tables/v2"); frame(bytes, identifier);
    frame(bytes, std::to_string(tables.size()));
    for (const auto& table : tables) {
        frame(bytes, table.name); frame(bytes, std::to_string(table.kind)); frame(bytes, table.primary_key);
        frame(bytes, std::to_string(table.columns.size()));
        for (const auto& column : table.columns) {
            frame(bytes, column.name); frame(bytes, std::to_string(int(column.type)));
            frame(bytes, column.nullable ? "1" : "0"); frame(bytes, std::to_string(column.collection));
            frame(bytes, column.indexed ? "1" : "0"); frame(bytes, column.fulltext_indexed ? "1" : "0");
            frame(bytes, column.link_target);
        }
    }
    Identity digest{}; util::sha256(bytes.data(), bytes.size(), digest.data()); return digest;
}

SessionAdmission::SessionAdmission(std::string identifier, std::vector<Table> tables)
    : m_identifier(std::move(identifier)), m_tables(validate(std::move(tables))), m_identity(fingerprint(m_identifier, m_tables)) {}

void SessionAdmission::validate_payload_path(const std::string& path)
{
    using R = SessionAdmissionError::Reason;
#ifdef _WIN32
    // The pinned Windows File implementation uses this same C++17 Unicode
    // path API. Do not enable std::filesystem on older Apple deployments.
    std::error_code error;
    auto status = std::filesystem::symlink_status(std::filesystem::u8path(path), error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw SessionAdmissionError(R::unreadable);
    if (std::filesystem::exists(status)) {
        if (!std::filesystem::is_regular_file(status))
            throw SessionAdmissionError(R::unsupported_configuration);
        auto links = std::filesystem::hard_link_count(std::filesystem::u8path(path), error);
        if (error) throw SessionAdmissionError(R::unreadable);
        if (links != 1) throw SessionAdmissionError(R::unsupported_configuration);
    }
    else if (std::filesystem::is_symlink(status)) {
        throw SessionAdmissionError(R::unsupported_configuration);
    }
#else
    struct stat status;
    if (::lstat(path.c_str(), &status) == 0) {
        if (!S_ISREG(status.st_mode) || status.st_nlink != 1)
            throw SessionAdmissionError(R::unsupported_configuration);
    }
    else if (errno != ENOENT) {
        throw SessionAdmissionError(R::unreadable);
    }
#endif
}

void SessionAdmission::inspect(const Group& group) const
{
    using R = SessionAdmissionError::Reason;
    for (const auto& expected : m_tables) {
        auto table = group.get_table(expected.name);
        if (!table) continue; // Supported older files may have no protected tables.
        if (table->size() != 0 || table->nb_unresolved() != 0) throw SessionAdmissionError(R::nonempty_table, expected.name);
        auto unknown = [&] { throw SessionAdmissionError(R::unknown_schema, expected.name); };
        if (unsigned(table->get_table_type()) != expected.kind || table->get_column_count() != expected.columns.size()) unknown();
        auto primary = table->get_primary_key_column();
        if ((primary ? std::string(table->get_column_name(primary)) : std::string{}) != expected.primary_key) unknown();
        for (const auto& column : expected.columns) {
            auto key = table->get_column_key(column.name);
            if (!key) unknown();
            unsigned collection = (key.is_list() ? 1 : 0) | (key.is_set() ? 2 : 0) | (key.is_dictionary() ? 4 : 0);
            if (table->get_column_type(key) != column.type || key.is_nullable() != column.nullable
                || collection != column.collection || table->has_search_index(key) != column.indexed
                || (table->search_index_type(key) == IndexType::Fulltext) != column.fulltext_indexed) unknown();
            if (key.is_dictionary() && table->get_dictionary_key_type(key) != type_String) unknown();
            if (column.type == type_Link) {
                auto target = table->get_link_target(key);
                if (!target || target->get_name() != column.link_target) unknown();
            }
        }
    }
}
} // namespace realm
