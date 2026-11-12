/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/db.hpp>
#include "test.hpp"

using namespace realm;

TEST(DB_getCoreFiles_adding_path)
{
    std::string path = "path/to/realm/files/";
    auto extension_lock = ".lock";
    auto extension_storage = "";
    auto extension_management = ".management";
    auto extension_note = ".note";
    auto extension_log = ".log";

    CHECK_EQUAL(DB::get_core_file(path, DB::CoreFileType::Lock), path + extension_lock);
    CHECK_EQUAL(DB::get_core_file(path, DB::CoreFileType::Storage), path + extension_storage);
    CHECK_EQUAL(DB::get_core_file(path, DB::CoreFileType::Management), path + extension_management);
    CHECK_EQUAL(DB::get_core_file(path, DB::CoreFileType::Note), path + extension_note);
    CHECK_EQUAL(DB::get_core_file(path, DB::CoreFileType::Log), path + extension_log);
}


// Session admission regressions use the public DB path, not direct predicate calls.
#include <realm/alloc_slab.hpp>
#include <realm/transaction.hpp>
#include <realm/history.hpp>
#include <realm/session_admission.hpp>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
std::shared_ptr<const SessionAdmission> db_admission_contract(std::string identifier = "db-admission-tests/v1")
{
    std::vector<SessionAdmission::Table> tables;
    for (auto name : {"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"}) {
        SessionAdmission::Table table;
        table.name = name;
        table.columns.push_back({"value", type_Int});
        tables.push_back(std::move(table));
    }
    return std::make_shared<const SessionAdmission>(std::move(identifier), std::move(tables));
}

std::string db_admission_bytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read admission test fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool db_admission_is_streaming(const std::string& path)
{
    SlabAlloc alloc;
    SlabAlloc::Config config;
    config.read_only = true;
    config.no_create = true;
    alloc.attach_file(path, config);
    return alloc.is_file_on_streaming_form();
}

void seed_db_admission_file(const std::string& path, int absent = -1, int nonempty = -1, int mismatch = -1)
{
    auto db = DB::create(path);
    auto write = db->start_write();
    int index = 0;
    for (auto name : {"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"}) {
        int current = index++;
        if (absent == 3 || current == absent) continue;
        auto table = write->add_table(name);
        table->add_column(type_Int, "value");
        if (current == nonempty) table->create_object();
        if (current == mismatch) table->add_column(type_String, "unexpected");
    }
    write->commit();
}
} // namespace

TEST(DB_SessionAdmission_AllEmptyAndEveryAbsentTable)
{
    for (int absent = -1; absent != 4; ++absent) {
        SHARED_GROUP_TEST_PATH(path);
        seed_db_admission_file(path, absent);
        DBOptions options;
        options.session_admission = db_admission_contract();
        auto db = DB::create(path, options);
        CHECK(db->session_admission_identity() == options.session_admission->identity());
        auto read = db->start_read();
        int index = 0;
        for (auto name : {"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"}) {
            auto table = read->get_table(name);
            CHECK(bool(table) == (absent != 3 && index++ != absent));
            if (table) CHECK_EQUAL(table->size(), 0);
        }
    }
}

TEST(DB_SessionAdmission_EachNonemptyTableDeniedWithoutFileEffects)
{
    for (int selected = 0; selected != 3; ++selected) {
        SHARED_GROUP_TEST_PATH(path);
        seed_db_admission_file(path, -1, selected);
        auto before = db_admission_bytes(path);
        auto inode = util::File::get_unique_id(path);
        auto modified = util::File::last_write_time(path);
        DBOptions options;
        options.session_admission = db_admission_contract();
        bool denied = false;
        try { auto db = DB::create(path, options); }
        catch (const SessionAdmissionError& error) {
            denied = true;
            CHECK(error.reason == SessionAdmissionError::Reason::nonempty_table);
            CHECK_EQUAL(error.table, std::vector<std::string>({"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"})[selected]);
        }
        CHECK(denied);
        CHECK(db_admission_bytes(path) == before);
        CHECK(inode == util::File::get_unique_id(path));
        CHECK_EQUAL(modified, util::File::last_write_time(path));
    }
}

TEST(DB_SessionAdmission_EachUnknownPhysicalSchemaDeniedWithoutFileEffects)
{
    for (int selected = 0; selected != 3; ++selected) {
        SHARED_GROUP_TEST_PATH(path);
        seed_db_admission_file(path, -1, -1, selected);
        auto before = db_admission_bytes(path);
        auto inode = util::File::get_unique_id(path);
        auto modified = util::File::last_write_time(path);
        DBOptions options;
        options.session_admission = db_admission_contract();
        bool denied = false;
        try { auto db = DB::create(path, options); }
        catch (const SessionAdmissionError& error) {
            denied = true;
            CHECK(error.reason == SessionAdmissionError::Reason::unknown_schema);
            CHECK_EQUAL(error.table, std::vector<std::string>({"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"})[selected]);
        }
        CHECK(denied);
        CHECK(db_admission_bytes(path) == before);
        CHECK(inode == util::File::get_unique_id(path));
        CHECK_EQUAL(modified, util::File::last_write_time(path));
    }
}

TEST(DB_SessionAdmission_ActualSessionFencesNilAndDifferentCohorts)
{
    SHARED_GROUP_TEST_PATH(path);
    DBOptions options;
    options.session_admission = db_admission_contract();
    auto first = DB::create(path, options);
    DBOptions equivalent;
    equivalent.session_admission = db_admission_contract(); // Equal values, distinct capability allocation.
    auto peer = DB::create(path, equivalent);
    CHECK(peer.get() != first.get());
    auto write = peer->start_write();
    write->add_table("unprotected")->add_column(type_Int, "value");
    write->commit();
    write.reset();
    CHECK(first->start_read()->get_table("unprotected"));
    CHECK_THROW(DB::create(path), SessionAdmissionError);
    DBOptions different;
    different.session_admission = db_admission_contract("other-cohort/v1");
    CHECK_THROW(DB::create(path, different), SessionAdmissionError);
    first.reset(); // The peer still owns the real session.
    CHECK_THROW(DB::create(path), SessionAdmissionError);
    CHECK(!DB::call_with_lock(path, [](const std::string&) {}));
    peer.reset();
    CHECK(DB::call_with_lock(path, [](const std::string&) {}));
    CHECK(DB::create(path)); // An ordinary open is allowed only after the admitted session ends.
}

TEST(DB_SessionAdmission_LifecycleRetainsLockAndCoordinationInode)
{
    SHARED_GROUP_TEST_PATH(path);
    // call_with_lock() alone may create a valid zero-length lock file, which
    // File::get_unique_id() deliberately does not identify on POSIX. Initialize
    // real shared metadata through DB before using that API as an inode witness.
    seed_db_admission_file(path);
    auto initialized_lock_id = util::File::get_unique_id(path.get_lock_path());
    CHECK(bool(initialized_lock_id));
    bool body_ran = false;
    std::optional<util::File::UniqueID> lock_id;
    CHECK(DB::call_with_lock(path, [&](const std::string&) {
        body_ran = true;
        CHECK(!DB::call_with_lock(path, [](const std::string&) {}));
        lock_id = util::File::get_unique_id(path.get_lock_path());
        CHECK(bool(lock_id));
        CHECK(lock_id == initialized_lock_id);
    }));
    CHECK(body_ran);
    CHECK(lock_id == util::File::get_unique_id(path.get_lock_path()));
    CHECK(DB::call_with_lock(path, [](const std::string&) {}));
}


namespace {
SessionAdmission::Table matrix_table()
{
    SessionAdmission::Table table;
    table.name = "class_AdmissionMatrix";
    table.columns.push_back({"value", type_Int});
    return table;
}

std::shared_ptr<const SessionAdmission> matrix_contract(const SessionAdmission::Table& table)
{
    return std::make_shared<const SessionAdmission>("physical-schema-matrix/v1",
        std::vector<SessionAdmission::Table>{table});
}

void seed_matrix_table(const std::string& path, const SessionAdmission::Table& shape)
{
    auto db = DB::create(path);
    auto write = db->start_write();
    auto target_a = write->add_table("class_TargetA");
    auto target_b = write->add_table("class_TargetB");
    auto table = write->add_table(shape.name, Table::Type(shape.kind));
    for (const auto& column : shape.columns) {
        std::optional<CollectionType> collection;
        if (column.collection == 1) collection = CollectionType::List;
        if (column.collection == 2) collection = CollectionType::Set;
        if (column.collection == 4) collection = CollectionType::Dictionary;
        auto key = column.type == type_Link
            ? table->add_column(*(column.link_target == "class_TargetA" ? target_a : target_b), column.name, collection)
            : table->add_column(column.type, column.name, column.nullable, collection);
        if (column.indexed) table->add_search_index(key);
        if (column.fulltext_indexed) table->add_search_index(key, IndexType::Fulltext);
        if (column.name == shape.primary_key) table->set_primary_key_column(key);
    }
    write->commit();
}
} // namespace

TEST(DB_SessionAdmission_UnresolvedPhysicalRowsDeniedWithoutFileEffects)
{
    SHARED_GROUP_TEST_PATH(path);
    auto shape = matrix_table();
    shape.primary_key = "value";
    shape.columns[0].indexed = true;
    seed_matrix_table(path, shape);
    {
        auto db = DB::create(path);
        auto write = db->start_write();
        auto table = write->get_table(shape.name);
        table->get_objkey_from_primary_key(Mixed{int64_t(7)});
        CHECK_EQUAL(table->size(), 0);
        CHECK_EQUAL(table->nb_unresolved(), 1);
        write->commit();
    }
    // Check the persisted state after the ordinary coordinated writer closes.
    {
        auto db = DB::create(path);
        auto read = db->start_read();
        auto table = read->get_table(shape.name);
        CHECK_EQUAL(table->size(), 0);
        CHECK_EQUAL(table->nb_unresolved(), 1);
    }
    auto before = db_admission_bytes(path);
    auto inode = util::File::get_unique_id(path);
    auto modified = util::File::last_write_time(path);
    DBOptions options;
    options.session_admission = matrix_contract(shape);
    bool denied = false;
    try { auto db = DB::create(path, options); }
    catch (const SessionAdmissionError& error) {
        denied = true;
        CHECK(error.reason == SessionAdmissionError::Reason::nonempty_table);
        CHECK_EQUAL(error.table, shape.name);
    }
    CHECK(denied);
    CHECK(db_admission_bytes(path) == before);
    CHECK(inode == util::File::get_unique_id(path));
    CHECK_EQUAL(modified, util::File::last_write_time(path));
    {
        auto db = DB::create(path);
        auto read = db->start_read();
        auto table = read->get_table(shape.name);
        CHECK_EQUAL(table->size(), 0);
        CHECK_EQUAL(table->nb_unresolved(), 1);
    }
}

TEST(DB_SessionAdmission_AllPhysicalSchemaFields)
{
    using Shape = SessionAdmission::Table;
    std::vector<std::pair<Shape, Shape>> cases;
    auto add_case = [&](Shape expected, Shape actual) { cases.emplace_back(std::move(expected), std::move(actual)); };
    {
        auto expected = matrix_table(), actual = expected;
        actual.kind = unsigned(Table::Type::Embedded);
        add_case(expected, actual);
    }
    {
        auto expected = matrix_table(), actual = expected;
        expected.primary_key = "value";
        expected.columns[0].indexed = true;
        actual.columns[0].indexed = true; // Only the primary-key identity differs.
        add_case(expected, actual);
    }
    {
        auto expected = matrix_table(), actual = expected;
        actual.columns[0].type = type_String;
        add_case(expected, actual);
    }
    {
        auto expected = matrix_table(), actual = expected;
        actual.columns[0].nullable = true;
        add_case(expected, actual);
    }
    for (unsigned expected_collection : {0, 1, 2, 4}) {
        for (unsigned actual_collection : {0, 1, 2, 4}) {
            if (expected_collection == actual_collection) continue;
            auto expected = matrix_table(), actual = expected;
            expected.columns[0].collection = expected_collection;
            actual.columns[0].collection = actual_collection;
            add_case(expected, actual);
        }
    }
    for (bool fulltext : {false, true}) {
        auto expected = matrix_table(), actual = expected;
        expected.columns[0].type = actual.columns[0].type = type_String;
        actual.columns[0].indexed = !fulltext;
        actual.columns[0].fulltext_indexed = fulltext;
        add_case(expected, actual);
        add_case(actual, expected); // Both missing and unexpected index kinds must refuse.
    }
    {
        auto expected = matrix_table(), actual = expected;
        expected.columns[0].type = actual.columns[0].type = type_Link;
        expected.columns[0].nullable = actual.columns[0].nullable = true;
        expected.columns[0].link_target = "class_TargetA";
        actual.columns[0].link_target = "class_TargetB";
        add_case(expected, actual);
    }
    for (const auto& shapes : cases) {
        SHARED_GROUP_TEST_PATH(expected_path);
        SHARED_GROUP_TEST_PATH(actual_path);
        seed_matrix_table(expected_path, shapes.first);
        seed_matrix_table(actual_path, shapes.second);
        DBOptions expected_options, actual_options;
        expected_options.session_admission = matrix_contract(shapes.first);
        actual_options.session_admission = matrix_contract(shapes.second);
        // Each side is a valid physical schema and valid contract, not a malformed-descriptor negative.
        { auto valid_expected = DB::create(expected_path, expected_options); }
        { auto valid_actual = DB::create(actual_path, actual_options); }
        auto bytes = db_admission_bytes(actual_path);
        auto inode = util::File::get_unique_id(actual_path);
        auto modified = util::File::last_write_time(actual_path);
        bool denied = false;
        try { auto wrong_shape = DB::create(actual_path, expected_options); }
        catch (const SessionAdmissionError& error) {
            denied = true;
            CHECK(error.reason == SessionAdmissionError::Reason::unknown_schema);
            CHECK_EQUAL(error.table, "class_AdmissionMatrix");
        }
        CHECK(denied);
        CHECK(bytes == db_admission_bytes(actual_path));
        CHECK(inode == util::File::get_unique_id(actual_path));
        CHECK_EQUAL(modified, util::File::last_write_time(actual_path));
        // The unchanged physical schema still admits its original contract after rejection.
        { auto unchanged_schema = DB::create(actual_path, actual_options); }
    }
}

TEST(DB_SessionAdmission_DescriptorNameBoundsAndEmbeddedNul)
{
    auto valid = matrix_table();
    CHECK(matrix_contract(valid));
    CHECK(SessionAdmission(std::string(256, 'a'), {valid}).identifier().size() == 256);
    for (const auto& identifier : {std::string{}, std::string(257, 'a'), std::string("ok\0suffix", 9)}) {
        CHECK_THROW((SessionAdmission(identifier, {valid})), SessionAdmissionError);
    }
    for (const auto& name : {std::string{}, std::string(257, 'a'), std::string("ok\0suffix", 9)}) {
        auto table = valid;
        table.name = name;
        CHECK_THROW(matrix_contract(table), SessionAdmissionError);
        table = valid;
        table.columns[0].name = name;
        CHECK_THROW(matrix_contract(table), SessionAdmissionError);
        table = valid;
        table.columns[0].type = type_Link;
        table.columns[0].nullable = true;
        table.columns[0].link_target = name;
        CHECK_THROW(matrix_contract(table), SessionAdmissionError);
        if (!name.empty()) {
            table = valid;
            table.primary_key = name;
            CHECK_THROW(matrix_contract(table), SessionAdmissionError);
        }
    }
    auto table = valid;
    table.name = std::string(256, 't');
    table.columns[0].name = std::string(256, 'c');
    CHECK(matrix_contract(table));
    table.columns[0].type = type_Link;
    table.columns[0].nullable = true;
    table.columns[0].link_target = std::string(256, 'l');
    CHECK(matrix_contract(table));
}

TEST(DB_SessionAdmission_DeniesBeforeStreamingHeaderNormalization)
{
    for (int selected = -1; selected != 3; ++selected) {
        SHARED_GROUP_TEST_PATH(path);
        {
            Group group;
            int index = 0;
            for (auto name : {"class_AdmissionA", "class_AdmissionB", "class_AdmissionC"}) {
                auto table = group.add_table(name);
                table->add_column(type_Int, "value");
                if (index++ == selected) table->create_object();
            }
            // The public copy writer creates a streaming header/footer. No
            // writable DB has attached to normalize this physical input yet.
            group.write(path);
        }
        auto before = db_admission_bytes(path);
        CHECK(before.substr(0, 8) == std::string(8, char(0xff)));
        CHECK(db_admission_is_streaming(path));
        auto inode = util::File::get_unique_id(path);
        auto modified = util::File::last_write_time(path);
        DBOptions options;
        options.session_admission = db_admission_contract();
        if (selected == -1) {
            auto admitted = DB::create(path, options);
            CHECK(admitted->start_read()->get_table("class_AdmissionA")->is_empty());
            admitted.reset();
            // Normalization selects the second top-ref slot; the first eight
            // bytes need not change. Check the actual physical header state.
            CHECK(!db_admission_is_streaming(path));
            CHECK(db_admission_bytes(path) != before);
            CHECK(util::File::get_unique_id(path) == inode);
        }
        else {
            bool denied = false;
            try { auto denied_db = DB::create(path, options); }
            catch (const SessionAdmissionError& error) {
                denied = true;
                CHECK(error.reason == SessionAdmissionError::Reason::nonempty_table);
            }
            CHECK(denied);
            CHECK(db_admission_bytes(path) == before);
            CHECK(util::File::get_unique_id(path) == inode);
            CHECK_EQUAL(util::File::last_write_time(path), modified);
        }
    }
}

#ifndef _WIN32
TEST(DB_SessionAdmission_TerminalPayloadAliasRejectedBeforeAttach)
{
    SHARED_GROUP_TEST_PATH(target);
    SHARED_GROUP_TEST_PATH(alias);
    SHARED_GROUP_TEST_PATH(absent);
    seed_db_admission_file(target);
    auto ordinary_owner = DB::create(target);
    CHECK_EQUAL(::symlink(target.c_str(), alias.c_str()), 0);
    auto bytes = db_admission_bytes(target);
    auto inode = util::File::get_unique_id(target);
    auto modified = util::File::last_write_time(target);
    auto lock_bytes = db_admission_bytes(target.get_lock_path());
    auto lock_inode = util::File::get_unique_id(target.get_lock_path());
    DBOptions guarded;
    guarded.session_admission = db_admission_contract();
    bool denied = false;
    try { auto invalid = DB::create(alias, guarded); }
    catch (const SessionAdmissionError& error) {
        denied = true;
        CHECK(error.reason == SessionAdmissionError::Reason::unsupported_configuration);
    }
    CHECK(denied);
    CHECK(db_admission_bytes(target) == bytes);
    CHECK(inode == util::File::get_unique_id(target));
    CHECK_EQUAL(modified, util::File::last_write_time(target));
    CHECK(db_admission_bytes(target.get_lock_path()) == lock_bytes);
    CHECK(lock_inode == util::File::get_unique_id(target.get_lock_path()));
    CHECK(!util::File::exists(alias.get_lock_path()));
    struct stat alias_status;
    CHECK_EQUAL(::lstat(alias.c_str(), &alias_status), 0);
    CHECK(S_ISLNK(alias_status.st_mode));
    // The real target's ordinary owner still fences its canonical name.
    CHECK_THROW(DB::create(target, guarded), SessionAdmissionError);
    ordinary_owner.reset();
    CHECK(DB::create(target, guarded));
    CHECK(DB::create(absent, guarded));
}
#endif

#ifndef _WIN32
TEST(DB_SessionAdmission_ModernAliasAttemptsRefusedForBothLiveOwnerDirections)
{
    for (bool admitted_owner : {false, true}) {
        SHARED_GROUP_TEST_PATH(target);
        SHARED_GROUP_TEST_PATH(alias);
        seed_db_admission_file(target);
        DBOptions guarded;
        guarded.session_admission = db_admission_contract();
        auto owner = DB::create(target, admitted_owner ? guarded : DBOptions{});
        CHECK_EQUAL(::symlink(target.c_str(), alias.c_str()), 0);
        auto bytes = db_admission_bytes(target);
        auto inode = util::File::get_unique_id(target);
        auto modified = util::File::last_write_time(target);
        auto lock_bytes = db_admission_bytes(target.get_lock_path());
        auto lock_inode = util::File::get_unique_id(target.get_lock_path());
        auto refused = [&](auto&& operation) {
            bool denied = false;
            try { operation(); }
            catch (const SessionAdmissionError& error) {
                denied = true;
                CHECK(error.reason == SessionAdmissionError::Reason::unsupported_configuration);
            }
            CHECK(denied);
        };
        // The opposite cohort must fail specifically for its terminal path,
        // before creating another session's adjacent lock or touching payload.
        refused([&] { auto db = DB::create(alias, admitted_owner ? DBOptions{} : guarded); });
        refused([&] { auto db = DB::create(make_in_realm_history(), alias); });
        refused([&] { auto db = DB::create(make_in_realm_history(), alias, guarded); });
        bool entered = false, deleted = false;
        refused([&] { DB::call_with_lock(alias, [&](const std::string&) { entered = true; }); });
        refused([&] { DB::delete_files(alias, &deleted); });
        refused([&] { DB::delete_files(alias, &deleted, true); });
        refused([&] { owner->write_copy(alias, nullptr); });
        CHECK(!entered);
        CHECK(!deleted);
        CHECK(db_admission_bytes(target) == bytes);
        CHECK(inode == util::File::get_unique_id(target));
        CHECK_EQUAL(modified, util::File::last_write_time(target));
        CHECK(db_admission_bytes(target.get_lock_path()) == lock_bytes);
        CHECK(lock_inode == util::File::get_unique_id(target.get_lock_path()));
        CHECK(!util::File::exists(alias.get_lock_path()));
        struct stat status;
        CHECK_EQUAL(::lstat(alias.c_str(), &status), 0);
        CHECK(S_ISLNK(status.st_mode));
        // A matching canonical peer retains its actual owner's cohort.
        CHECK(DB::create(target, admitted_owner ? guarded : DBOptions{}));
        CHECK_THROW(DB::create(target, admitted_owner ? DBOptions{} : guarded), SessionAdmissionError);
        CHECK(!DB::call_with_lock(target, [](const std::string&) {}));
        owner.reset();
        CHECK(DB::call_with_lock(target, [&](const std::string& owned) { DB::delete_files(owned); }));
        CHECK(lock_inode == util::File::get_unique_id(target.get_lock_path()));
        CHECK(!util::File::exists(target));
    }
}

TEST(DB_SessionAdmission_HardLinkedPayloadsCannotSplitCoordination)
{
    SHARED_GROUP_TEST_PATH(target);
    SHARED_GROUP_TEST_PATH(alias);
    seed_db_admission_file(target);
    CHECK_EQUAL(::link(target.c_str(), alias.c_str()), 0);
    auto bytes = db_admission_bytes(target);
    auto inode = util::File::get_unique_id(target);
    DBOptions guarded;
    guarded.session_admission = db_admission_contract();
    CHECK_THROW(DB::create(target), SessionAdmissionError);
    CHECK_THROW(DB::create(alias), SessionAdmissionError);
    CHECK_THROW(DB::create(alias, guarded), SessionAdmissionError);
    CHECK_THROW(DB::call_with_lock(alias, [](const std::string&) {}), SessionAdmissionError);
    CHECK_THROW(DB::delete_files(alias), SessionAdmissionError);
    CHECK(db_admission_bytes(target) == bytes);
    CHECK(inode == util::File::get_unique_id(target));
    CHECK(inode == util::File::get_unique_id(alias));
    CHECK(!util::File::exists(alias.get_lock_path()));
    // Immutable, nonpromotable snapshots remain outside writer coordination.
    DBOptions immutable;
    immutable.is_immutable = true;
    CHECK(DB::create(alias, immutable));
}
#endif
