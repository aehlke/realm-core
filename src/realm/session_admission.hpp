#ifndef REALM_SESSION_ADMISSION_HPP
#define REALM_SESSION_ADMISSION_HPP

#include <realm/data_type.hpp>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace realm {
class Group;

class SessionAdmissionError : public std::runtime_error {
public:
    enum class Reason { incompatible_session = 1, invalid_descriptor, unsupported_configuration, unreadable,
                        unknown_schema, nonempty_table, busy };
    SessionAdmissionError(Reason reason, std::string table = {});
    const Reason reason;
    const std::string table;
};

// A value-only contract. No application callback can substitute another predicate.
// Identity includes the recipe, identifier and every exact physical schema field.
class SessionAdmission final {
public:
    using Identity = std::array<unsigned char, 32>;
    struct Column {
        std::string name;
        DataType type;
        bool nullable = false;
        unsigned collection = 0; // 0 scalar, 1 list, 2 set, 4 string-key dictionary
        bool indexed = false;
        bool fulltext_indexed = false;
        std::string link_target;
    };
    struct Table {
        std::string name;
        unsigned kind = 0;
        std::string primary_key;
        std::vector<Column> columns;
    };
    SessionAdmission(std::string identifier, std::vector<Table> tables);
    const std::string& identifier() const noexcept { return m_identifier; }
    const Identity& identity() const noexcept { return m_identity; }
    void inspect(const Group&) const;
    // Mutable payloads must be absent or regular files with one directory entry.
    // Terminal symlinks/hard links can split the adjacent lock namespace; refuse
    // them for nil and admitted writers and consuming lifecycle entrypoints.
    // Parent-directory aliases are supported when payload and lock are shared.
    // Check before effects and again under actual retained ownership. This is
    // advisory coordination for stable paths, not protection from path replacement.
    static void validate_payload_path(const std::string&);
    static Identity identity_of(const std::shared_ptr<const SessionAdmission>& value) noexcept
    { return value ? value->identity() : Identity{}; }
private:
    const std::string m_identifier;
    const std::vector<Table> m_tables;
    const Identity m_identity;
    static std::vector<Table> validate(std::vector<Table>);
    static Identity fingerprint(const std::string&, const std::vector<Table>&);
};
} // namespace realm
#endif
