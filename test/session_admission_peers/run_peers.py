#!/usr/bin/env python3
"""Use separately built old and modern executables; never build or edit SDKs.
Arguments: OLD_EXE MODERN_EXE OLD_PROVENANCE MODERN_PROVENANCE EMPTY_RESULTS_DIR.
All supplied SDK/build roots and the results directory must already exist.
"""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys

EVIDENCE = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def witness(path):
    stat = path.stat()
    return {"sha256": sha(path), "device": stat.st_dev, "inode": stat.st_ino,
            "mtime_ns": stat.st_mtime_ns, "size": stat.st_size}


def run(executable, *args, expected=(0, "ACCEPTED")):
    completed = subprocess.run([str(executable), *map(str, args)], text=True,
                               capture_output=True, timeout=25)
    actual = (completed.returncode, completed.stdout.strip())
    assert actual == expected, (actual, expected, completed.stderr)
    EVIDENCE.append({"binary": str(executable), "arguments": list(map(str, args)),
                     "returncode": completed.returncode, "stdout": completed.stdout.strip()})
    return completed.stdout.strip()


def line_before(process):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        assert selector.select(25), "peer handshake timed out"
        line = process.stdout.readline().strip()
        assert line, "peer exited before handshake"
        return line


def release(process):
    assert process.poll() is None, "owner exited before release"
    process.stdin.write("release\n")
    process.stdin.flush()
    assert line_before(process) == "RELEASED"
    assert process.wait(timeout=25) == 0


@contextlib.contextmanager
def held(executable, path, cohort, mode="--hold", ready="READY"):
    arguments = [str(executable), mode, str(path)] + ([cohort] if cohort is not None else [])
    process = subprocess.Popen(arguments, text=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, bufsize=1)
    try:
        assert line_before(process) == ready
        EVIDENCE.append({"holder": str(executable), "cohort": cohort, "mode": mode, "path": str(path), "ready": ready})
        yield process
    finally:
        # Only this exact Popen-owned child is signalled, and only after bounded EOF recovery.
        if process.poll() is None:
            process.stdin.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        process.stdout.close()
        process.stderr.close()
        if not process.stdin.closed:
            process.stdin.close()


def denied_without_effect(executable, path, cohort):
    before = witness(path)
    run(executable, "--probe", path, cohort, expected=(2, "DENIED"))
    after = witness(path)
    assert after == before, ("denied peer changed original", before, after)
    EVIDENCE.append({"denied_cohort": cohort, "unchanged_file": after})


def ordering(holder_exe, holder_cohort, contender_exe, contender_cohort, path):
    with held(holder_exe, path, holder_cohort) as owner:
        denied_without_effect(contender_exe, path, contender_cohort)
        denied_without_effect(contender_exe, path, contender_cohort)
        release(owner)
    # A previously rejected peer must work after the last actual owner releases its DB.
    run(contender_exe, "--probe", path, contender_cohort)


def alias_denied_without_effect(modern, alias, target, cohort):
    # Follow the payload only for a witness; the supplied API path remains the alias.
    before = witness(target)
    lock = Path(str(target) + ".lock")
    lock_before = witness(lock)
    alias_before = os.readlink(alias)
    assert not Path(str(alias) + ".lock").exists()
    run(modern, "--probe", alias, cohort, expected=(3, "ALIAS_DENIED"))
    assert witness(target) == before, "alias refusal changed the canonical payload"
    assert witness(lock) == lock_before, "alias refusal changed canonical coordination"
    assert alias.is_symlink() and os.readlink(alias) == alias_before
    assert not Path(str(alias) + ".lock").exists(), "alias refusal created a separate lock"
    EVIDENCE.append({"alias_cohort": cohort, "alias_path": str(alias),
                     "unchanged_file": before, "unchanged_coordination": lock_before})


def provenance(manifest_path, executable, role):
    entries = {}
    for line in manifest_path.read_text().splitlines():
        key, value = line.split("=", 1)
        assert key not in entries, "duplicate provenance key"
        entries[key] = value
    assert entries["role"] == role
    assert entries["source_revision"]
    assert entries["db_sha256"] == entries["expected_db_sha256"]
    assert Path(entries["executable"]).resolve(strict=True) == executable
    root = Path(entries["source_root"]).resolve(strict=True)
    hashes = {key.removeprefix("source_sha256."): value for key, value in entries.items()
              if key.startswith("source_sha256.")}
    for relative, expected_hash in hashes.items():
        assert sha(root / relative) == expected_hash, "SDK source changed after configure"
    assert hashes["src/realm/db.cpp"] == entries["db_sha256"]
    assert (root / "src/realm/session_admission.hpp").exists() == (role == "modern")
    assert entries["peer_sha256"] == sha(Path(__file__).with_name("peer.cpp"))
    run(executable, "--identity", expected=(0, role + " " + entries["db_sha256"]))
    compile_database = manifest_path.parent / "compile_commands.json"
    commands = json.loads(compile_database.read_text())
    compiled = {}
    for item in commands:
        source = Path(item["file"])
        if not source.is_absolute():
            source = Path(item["directory"]) / source
        compiled[source.resolve(strict=True)] = item.get("command", " ".join(item.get("arguments", [])))
    assert root / "src/realm/db.cpp" in compiled
    assert root / "src/realm/object-store/impl/realm_coordinator.cpp" in compiled
    peer_path = Path(__file__).resolve().with_name("peer.cpp")
    peer_command = compiled[peer_path]
    assert ("REALM_ADMISSION_PEER_MODERN=1" in peer_command) == (role == "modern")
    assert entries["db_sha256"] in peer_command
    entries["compile_commands_sha256"] = sha(compile_database)
    entries["binary_sha256"] = sha(executable)
    for key in ("storage_library", "objectstore_library"):
        entries[key + "_sha256"] = sha(Path(entries[key]).resolve(strict=True))
    return entries


def main():
    assert len(sys.argv) == 6, __doc__
    old, modern, old_manifest, modern_manifest, results = (
        Path(value).resolve(strict=True) for value in sys.argv[1:])
    assert not os.path.samefile(old, modern), "old/new must be separately linked executables"
    assert results.is_dir() and not any(results.iterdir()), "provide a fresh empty results directory"
    old_source = provenance(old_manifest, old, "old")
    modern_source = provenance(modern_manifest, modern, "modern")
    assert old_source["source_root"] != modern_source["source_root"]
    assert old_source["db_sha256"] != modern_source["db_sha256"]
    assert old_source["storage_library"] != modern_source["storage_library"]
    assert old_source["objectstore_library"] != modern_source["objectstore_library"]
    try:
        # Modern nil, different-value admission, and raw generic mutable DB: both ownership orderings.
        for cohort in ("nil", "different", "generic"):
            ordering(modern, "same", modern, cohort, results / ("admitted-first-" + cohort + ".realm"))
            ordering(modern, cohort, modern, "same", results / (cohort + "-first.realm"))

        # Equal-value admissions, independently constructed in two actual processes.
        path = results / "matching-modern-concurrent.realm"
        with held(modern, path, "same") as first:
            with held(modern, path, "same") as second:
                run(modern, "--count", path, "same", expected=(0, "COUNT 2"))
                for cohort in ("nil", "different", "generic"):
                    denied_without_effect(modern, path, cohort)
                release(first)
                for cohort in ("nil", "different", "generic"):
                    denied_without_effect(modern, path, cohort) # Second DB still owns the session.
                release(second)
        for cohort in ("different", "nil", "generic"):
            run(modern, "--probe", path, cohort)

        # Prospective modern alias refusal in both inverse directions. Every mode
        # goes through the real supplied path; nil and raw generic are not stand-ins
        # for the separately linked pristine-old same-path matrix below.
        for cohort in ("same", "nil", "generic"):
            target = results / ("canonical-owner-alias-" + cohort + ".realm")
            alias = results / ("canonical-owner-alias-" + cohort + "-link.realm")
            with held(modern, target, "same") as owner:
                alias.symlink_to(target)
                alias_denied_without_effect(modern, alias, target, cohort)
                release(owner)
            # Inverse attempted ordering: the alias cannot establish ownership,
            # even when no canonical owner exists. Canonical ownership then works.
            alias_denied_without_effect(modern, alias, target, cohort)
            with held(modern, target, "same") as owner:
                alias_denied_without_effect(modern, alias, target, cohort)
                release(owner)
        # Guarded alias after an ordinary canonical owner is the other cohort
        # direction: it must fail for the alias itself before a new lock appears.
        target = results / "ordinary-owner-guarded-alias.realm"
        alias = results / "ordinary-owner-guarded-alias-link.realm"
        with held(old, target, "legacy") as owner:
            alias.symlink_to(target)
            alias_denied_without_effect(modern, alias, target, "same")
            release(owner)

        # These are separately linked pristine old SDK peers, never simulated modern nil configs.
        ordering(old, "legacy", modern, "same", results / "pristine-old-first.realm")
        ordering(modern, "same", old, "legacy", results / "pristine-old-second.realm")

        # The old immutable reader is outside admission. It uses actual public throwing APIs and
        # then remains alive while modern establishes real DB ownership without a user write.
        path = results / "pristine-old-immutable.realm"
        with held(old, path, "legacy") as seed:
            release(seed)
        before = witness(path)
        run(old, "--immutable", path, expected=(0, "IMMUTABLE_VERIFIED"))
        assert witness(path) == before
        with held(old, path, None, mode="--immutable-held", ready="IMMUTABLE_READY") as immutable:
            assert witness(path) == before
            with held(modern, path, "same", mode="--retain") as owner:
                assert witness(path) == before, "retaining modern owner changed old immutable snapshot"
                immutable.stdin.write("close-and-probe\n")
                immutable.stdin.flush()
                assert line_before(immutable) == "DENIED"
                assert immutable.wait(timeout=25) == 2
                assert witness(path) == before
                EVIDENCE.append({"old_same_process_immutable_to_ordinary": "DENIED", "unchanged_file": witness(path)})
                denied_without_effect(old, path, "legacy")
                for cohort in ("nil", "different", "generic"):
                    denied_without_effect(modern, path, cohort)
                release(owner)
        run(old, "--probe", path, "legacy")
        result = {"status": "passed", "old": old_source, "modern": modern_source, "events": EVIDENCE}
        (results / "peer-evidence.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"status": "passed", "evidence": str(results / "peer-evidence.json")}))
    except BaseException as error:
        (results / "peer-failure.json").write_text(json.dumps({"status": "failed", "error": repr(error),
            "old": old_source, "modern": modern_source, "events": EVIDENCE}, indent=2) + "\n")
        raise


if __name__ == "__main__":
    main()
