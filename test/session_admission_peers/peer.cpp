// Built only by the sibling standalone CMake project, once per supplied SDK root.
#include <realm/db.hpp>
#include <realm/transaction.hpp>
#include <realm/history.hpp>
#include <realm/object-store/shared_realm.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/property.hpp>
#include <realm/util/file.hpp>
#ifdef REALM_ADMISSION_PEER_MODERN
#include <realm/session_admission.hpp>
#endif
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
std::string bytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read peer Realm");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

realm::Realm::Config config_for(const std::string& path, const std::string& cohort)
{
    realm::Realm::Config config;
    config.path = path;
    config.schema_version = 0;
    config.schema = realm::Schema{{"PeerWritable", {{"value", realm::PropertyType::Int}}}};
    config.automatic_change_notifications = false;
#ifdef REALM_ADMISSION_PEER_MODERN
    if (cohort == "same" || cohort == "different") {
        realm::SessionAdmission::Table table;
        table.name = "class_Protected";
        table.columns.push_back({"value", realm::type_Int});
        config.session_admission = std::make_shared<const realm::SessionAdmission>(
            cohort == "same" ? "sdk-peer-cohort/v1" : "sdk-peer-other-cohort/v1",
            std::vector<realm::SessionAdmission::Table>{std::move(table)});
    }
    else if (cohort != "nil" && cohort != "generic") throw std::runtime_error("invalid modern cohort");
#else
    if (cohort != "legacy" && cohort != "immutable") throw std::runtime_error("invalid old cohort");
#endif
    return config;
}

realm::SharedRealm verified_immutable(const std::string& path)
{
    auto original = bytes(path);
    auto inode = realm::util::File::get_unique_id(path);
    auto modified = realm::util::File::last_write_time(path);
    auto config = config_for(path, "immutable");
    config.schema_mode = realm::SchemaMode::Immutable;
    auto reader = realm::Realm::get_shared_realm(config);
    auto schema_version = reader->schema_version();
    auto original_rows = reader->read_group().get_table("class_PeerWritable")->size();
    bool sync_denied = false;
    try { reader->begin_transaction(); }
    catch (const realm::WrongTransactionState& error) {
        if (std::string(error.what()) != "Can't perform transactions on read-only Realms.") throw;
        sync_denied = true;
    }
    if (!sync_denied) {
        if (reader->is_in_transaction()) reader->cancel_transaction();
        throw std::runtime_error("immutable public Realm::begin_transaction was allowed");
    }
    bool async_denied = false, callback_ran = false;
    try { reader->async_begin_transaction([&] { callback_ran = true; }); }
    catch (const realm::WrongTransactionState& error) {
        if (std::string(error.what()) != "Can't perform transactions on read-only Realms.") throw;
        async_denied = true;
    }
    if (!async_denied || callback_ran) throw std::runtime_error("immutable public async write was allowed");
    bool copy_denied = false;
    try { reader->read_group().write(path); } // Group::write uses create_Must for an existing original.
    catch (const realm::FileAccessError& error) {
        if (error.code() != realm::ErrorCodes::FileAlreadyExists) throw;
        copy_denied = true;
    }
    if (!copy_denied || reader->is_in_transaction() || bytes(path) != original
        || realm::util::File::get_unique_id(path) != inode
        || realm::util::File::last_write_time(path) != modified
        || reader->schema_version() != schema_version
        || reader->read_group().get_table("class_PeerWritable")->size() != original_rows)
        throw std::runtime_error("immutable write/copy changed the original");
    return reader;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--identity") {
        std::cout << REALM_PEER_ROLE << " " << REALM_PEER_DB_SHA256 << std::endl;
        return 0;
    }
    try {
        if (argc == 3 && (std::string(argv[1]) == "--immutable" || std::string(argv[1]) == "--immutable-held")) {
#ifndef REALM_ADMISSION_PEER_MODERN
            auto reader = verified_immutable(argv[2]);
            if (std::string(argv[1]) == "--immutable") {
                std::cout << "IMMUTABLE_VERIFIED" << std::endl;
                return 0;
            }
            std::cout << "IMMUTABLE_READY" << std::endl;
            std::string command;
            if (!std::getline(std::cin, command) || command != "close-and-probe") return 65;
            reader.reset(); // Drop immutable DB/coordinator before the ordinary open: no permissions-cache false positive.
            auto ordinary = realm::Realm::get_shared_realm(config_for(argv[2], "legacy"));
            std::cout << "ACCEPTED" << std::endl;
            return 0;
#else
            throw std::runtime_error("immutable acceptance must use the pristine old executable");
#endif
        }
        if (argc != 4) return 64;
        std::string mode = argv[1], path = argv[2], cohort = argv[3];
        if (mode != "--hold" && mode != "--retain" && mode != "--probe" && mode != "--count") return 64;
        auto config = config_for(path, cohort);
        realm::SharedRealm owner;
        realm::DBRef generic;
#ifdef REALM_ADMISSION_PEER_MODERN
        if (cohort == "generic") {
            // Match Object Store's real history, so a history mismatch cannot masquerade as admission denial.
            if (mode == "--hold") { auto seed = realm::Realm::get_shared_realm(config); }
            generic = realm::DB::create(realm::make_in_realm_history(), path);
        }
        else
#endif
        { owner = realm::Realm::get_shared_realm(config); }
        if (mode == "--probe") {
            std::cout << "ACCEPTED" << std::endl;
            return 0;
        }
        if (mode == "--count") {
            auto count = owner ? owner->read_group().get_table("class_PeerWritable")->size()
                               : generic->start_read()->get_table("class_PeerWritable")->size();
            std::cout << "COUNT " << count << std::endl;
            return 0;
        }
        if (mode == "--retain") {
            if (!owner) throw std::runtime_error("retain requires a public Realm owner");
            // No user write while the accepted old immutable advisory reader still maps this file.
        }
        else if (owner) {
            owner->begin_transaction();
            owner->read_group().get_table("class_PeerWritable")->create_object();
            owner->commit_transaction();
        }
        else {
            auto write = generic->start_write();
            write->get_table("class_PeerWritable")->create_object();
            write->commit();
        }
        std::cout << "READY" << std::endl;
        std::string command;
        if (!std::getline(std::cin, command) || command != "release") return 65;
        owner.reset();
        generic.reset();
        std::cout << "RELEASED" << std::endl;
        return 0;
    }
#ifdef REALM_ADMISSION_PEER_MODERN
    catch (const realm::SessionAdmissionError& error) {
        if (error.reason == realm::SessionAdmissionError::Reason::unsupported_configuration) {
            std::cout << "ALIAS_DENIED" << std::endl;
            return 3;
        }
        if (error.reason != realm::SessionAdmissionError::Reason::incompatible_session) {
            std::cerr << error.what() << std::endl;
            return 70;
        }
        std::cout << "DENIED" << std::endl;
        return 2;
    }
#endif
    catch (const realm::FileAccessError& error) {
        if (error.code() == realm::ErrorCodes::IncompatibleLockFile) {
            std::cout << "DENIED" << std::endl;
            return 2;
        }
        std::cerr << error.what() << std::endl;
        return 70;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 70;
    }
}
