"""egraph status: the updates egraph watch last planned, read without planning."""

import json

from test_build import add_package
from test_refresh import egraph, scenario_system


def status_file(system):
    return system[1].parent / "status.json"


def test_status_update_plans_and_status_reads_it_back(mutable_playground, tmp_path):
    system = scenario_system(mutable_playground, tmp_path, "updates")
    missing = egraph(system, "status")
    assert missing.returncode == 1
    assert missing.stderr == (
        f"egraph: status: no status file at {status_file(system)}; egraphd writes it as it "
        "refreshes the stores, or egraph status --update now\n"
    )
    updated = egraph(system, "status", "--update")
    assert updated.returncode == 0, updated.stderr
    written = json.loads(status_file(system).read_text())
    updates = egraph(system, *written["command"].split()).stdout.splitlines()
    assert written["lines"] == updates != []
    fields = dict(line.split("\t", 1) for line in updated.stdout.splitlines())
    assert fields["upgrades"] == str(written["counts"]["upgrades"])
    assert fields["current"] == "yes"
    assert updated.stdout == egraph(system, "status").stdout
    document = json.loads(egraph(system, "status", "--json").stdout)
    assert document.pop("current") is True
    assert document == written
    human = egraph(system, "--layout", "human", "status").stdout
    assert human.splitlines()[-1] == "planned just now"


def test_a_status_planned_before_the_stores_changed_is_not_current(
    mutable_playground, tmp_path
):
    system = scenario_system(mutable_playground, tmp_path, "updates")
    assert egraph(system, "status", "--update").returncode == 0
    before = status_file(system).read_text()
    add_package(system[0], "dev-libs/alt-b-1")
    assert egraph(system, "refresh").returncode == 0
    # status only reads.
    fields = dict(
        line.split("\t", 1) for line in egraph(system, "status").stdout.splitlines()
    )
    assert fields["current"] == "no"
    assert status_file(system).read_text() == before
    human = egraph(system, "--layout", "human", "status").stdout
    assert human.splitlines()[-1].endswith(", before the stores last changed")


def test_a_broken_status_file_says_so(mutable_playground, tmp_path):
    system = scenario_system(mutable_playground, tmp_path, "reference")
    status_file(system).parent.mkdir(parents=True, exist_ok=True)
    status_file(system).write_text('{"format": 99}')
    result = egraph(system, "status")
    assert result.returncode == 1
    assert result.stderr == (
        f"egraph: status: {status_file(system)}: format 99, from another egraph version\n"
    )
