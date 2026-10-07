"""The system store's history: the generations refreshes replace, kept in var/lib/egraph."""

import calendar
import json
import os
import shutil
import subprocess
import sys
import time

import portage
import pytest

from test_build import add_package, add_to_world, age, vdb

EGRAPH = os.environ.get("EGRAPH")

pytestmark = pytest.mark.skipif(
    not EGRAPH, reason="set EGRAPH to the egraph binary (meson test does)"
)

BUILDER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORTAGE_LIB = os.path.dirname(os.path.dirname(portage.__file__))
DAY = 24 * 60 * 60


@pytest.fixture
def system(mutable_playground, tmp_path):
    playground = mutable_playground("reference")
    age(playground.eroot)
    builder = tmp_path / "egraph-build"
    builder.write_text(
        "#!/bin/sh\n"
        f'PYTHONPATH="{BUILDER_DIR}:{PORTAGE_LIB}" exec "{sys.executable}" -m egraph_build "$@"\n'
    )
    builder.chmod(0o755)
    return playground, builder


def egraph(system, *args):
    """egraph on the playground's system store, as root or the egraphd user runs it."""
    playground, builder = system
    return subprocess.run(
        [
            EGRAPH,
            "--config-root",
            playground.eroot,
            "--eprefix",
            playground.eprefix,
            "--builder",
            str(builder),
            *args,
        ],
        capture_output=True,
        text=True,
        env=dict(playground.settings.environ(), EGRAPH_STRICT="1"),
    )


def refresh(system):
    result = egraph(system, "refresh")
    assert (result.returncode, result.stderr) == (0, ""), result.stderr
    return result


def store(system):
    return os.path.join(system[0].eroot, "var/cache/egraph/installed.egraph")


def history(system):
    return os.path.join(system[0].eroot, "var/lib/egraph")


def generations(system):
    directory = history(system)
    if not os.path.isdir(directory):
        return []
    return sorted(
        name for name in os.listdir(directory) if name.startswith("installed-")
    )


def generation_name(seconds):
    return time.strftime("installed-%Y%m%dT%H%M%SZ.egraph", time.gmtime(seconds))


def log(system):
    path = os.path.join(history(system), "history.log")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(line) for line in f]


def settings(system, text):
    path = os.path.join(system[0].eroot, "etc/egraph/egraph.conf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


@pytest.mark.parametrize(
    "change",
    [
        lambda p: add_package(p, "dev-libs/alt-b-1"),
        lambda p: shutil.rmtree(vdb(p, "dev-libs/cond-1")),
        lambda p: add_to_world(p, "app-misc/old"),
    ],
    ids=["merged", "uninstalled", "world"],
)
def test_a_refresh_that_changes_the_system_keeps_the_store_it_replaced(system, change):
    refresh(system)
    assert generations(system) == []
    with open(store(system), "rb") as f:
        before = f.read()
    change(system[0])
    refresh(system)
    [kept] = generations(system)
    with open(os.path.join(history(system), kept), "rb") as f:
        assert f.read() == before
    assert os.stat(os.path.join(history(system), kept)).st_mode & 0o777 == 0o644


def test_a_refresh_that_leaves_the_installed_packages_keeps_nothing(system):
    refresh(system)
    with open(os.path.join(system[0].eroot, "etc/portage/make.conf"), "a") as f:
        f.write('EGRAPH_UNUSED="1"\n')
    refresh(system)
    assert egraph(system, "rebuild").returncode == 0
    assert generations(system) == []


def test_no_generations_with_history_days_zero_but_the_log_goes_on(system):
    settings(system, "history_days = 0\n")
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    refresh(system)
    assert generations(system) == []
    assert [event["cpv"] for event in log(system)] == ["dev-libs/alt-b-1"]


def test_broken_settings_warn_and_keep_no_generations(system):
    settings(system, "history = 3\n")
    assert egraph(system, "refresh").returncode == 0
    add_package(system[0], "dev-libs/alt-b-1")
    result = egraph(system, "refresh")
    assert result.returncode == 0
    assert result.stderr.endswith(
        "egraph.conf: line 1: unknown setting history, so no generations are kept\n"
    )
    assert result.stderr.count("\n") == 1
    assert generations(system) == []


def test_generations_are_thinned_by_age(system):
    refresh(system)
    now = time.time()
    directory = history(system)
    os.makedirs(directory)
    ancient = generation_name(now - 100 * DAY)
    day = (int(now) // DAY - 10) * DAY
    morning, evening = generation_name(day + 8 * 3600), generation_name(day + 20 * 3600)
    for name in [ancient, morning, evening]:
        with open(os.path.join(directory, name), "wb") as f:
            f.write(b"old")
    add_package(system[0], "dev-libs/alt-b-1")
    refresh(system)
    kept = generations(system)
    assert ancient not in kept and morning not in kept
    assert evening in kept and len(kept) == 2


def test_an_unwritable_history_is_skipped_quietly(system):
    refresh(system)
    os.makedirs(history(system))
    os.chmod(history(system), 0o555)
    try:
        add_package(system[0], "dev-libs/alt-b-1")
        refresh(system)
        assert generations(system) == []
    finally:
        os.chmod(history(system), 0o755)


def test_a_store_named_with_store_keeps_no_history(system, tmp_path):
    named = str(tmp_path / "installed.egraph")
    assert egraph(system, "--store", named, "refresh").returncode == 0
    add_package(system[0], "dev-libs/alt-b-1")
    assert egraph(system, "--store", named, "refresh").returncode == 0
    assert generations(system) == []


def test_the_log_records_merges_at_their_merge_time_and_uninstalls_when_found(system):
    playground = system[0]
    refresh(system)
    assert log(system) == []
    add_package(playground, "dev-libs/alt-b-1", COUNTER="7")
    merged = int(time.time()) - 600
    os.utime(vdb(playground, "dev-libs/alt-b-1", "COUNTER"), (merged, merged))
    refresh(system)
    assert log(system) == [
        {"cpv": "dev-libs/alt-b-1", "event": "merged", "time": merged}
    ]
    refresh(system)
    assert len(log(system)) == 1
    before = time.time()
    shutil.rmtree(vdb(playground, "dev-libs/alt-b-1"))
    refresh(system)
    [_, gone] = log(system)
    assert (gone["cpv"], gone["event"]) == ("dev-libs/alt-b-1", "uninstalled")
    assert before - 1 <= gone["time"] <= time.time()
    assert (
        os.stat(os.path.join(history(system), "history.log")).st_mode & 0o777 == 0o644
    )


def generation_time(name):
    return calendar.timegm(time.strptime(name, "installed-%Y%m%dT%H%M%SZ.egraph"))


def test_a_generation_is_named_by_when_the_system_left_it(system):
    refresh(system)
    time.sleep(1.1)
    left = int(time.time())
    add_package(system[0], "dev-libs/alt-b-1")
    refresh(system)
    [kept] = generations(system)
    assert generation_time(kept) >= left


def diff(system, *args):
    result = egraph(system, "diff", *args)
    assert result.returncode == 0, result.stderr
    return result


def test_diff_without_generations_says_so(system):
    refresh(system)
    result = egraph(system, "diff")
    assert result.returncode == 1
    assert result.stderr.startswith(
        f"egraph: diff: no generations in {history(system)} yet"
    )


def test_diff_shows_what_the_last_change_did(system):
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    shutil.rmtree(vdb(system[0], "dev-libs/cond-1"))
    add_to_world(system[0], "app-misc/old")
    assert diff(system).stdout == (
        "\tnew\tdev-libs/alt-b-1\t\n"
        "dev-libs/cond-1\tuninstall\t\t\n"
        "@selected\tadded\tapp-misc/old\n"
    )


def test_diff_by_age_and_date(system):
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    refresh(system)
    assert diff(system, "1h").stdout == "\tnew\tdev-libs/alt-b-1\t\n"
    # Nothing has changed since tomorrow.
    tomorrow = time.strftime("%Y-%m-%d", time.localtime(time.time() + DAY))
    assert diff(system, tomorrow).stdout == ""
    [kept] = generations(system)
    assert diff(system, kept).stdout == "\tnew\tdev-libs/alt-b-1\t\n"


def test_diff_before_the_history_says_since_when(system):
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    refresh(system)
    result = diff(system, "2000-01-01")
    assert result.stdout == "\tnew\tdev-libs/alt-b-1\t\n"
    assert result.stderr.startswith("egraph: diff: the history starts at ")


def test_diff_as_json(system):
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    document = json.loads(diff(system, "--json").stdout)
    [kept] = generations(system)
    assert document["changes"] == [
        {"after": "dev-libs/alt-b-1", "change": "new", "use": []}
    ]
    assert document["generation"] == kept
    assert document["since"] <= time.time()


def test_diff_takes_only_ages_dates_and_generations(system):
    result = egraph(system, "diff", "yesterday")
    assert result.returncode == 2
    assert "not yesterday" in result.stderr


def test_history_prints_the_log_as_lines(system):
    refresh(system)
    add_package(system[0], "dev-libs/alt-b-1")
    result = egraph(system, "history")
    assert result.returncode == 0, result.stderr
    assert [json.loads(line) for line in result.stdout.splitlines()] == log(system)
    assert [event["cpv"] for event in log(system)] == ["dev-libs/alt-b-1"]
    later = time.strftime("%Y-%m-%d", time.localtime(time.time() + DAY))
    assert egraph(system, "history", later).stdout == ""
    assert egraph(system, "history", "app-misc/old").stdout == ""


def test_history_shows_what_pulled_a_package_in(system):
    playground = system[0]
    refresh(system)
    add_package(playground, "dev-libs/alt-b-1")
    add_to_world(playground, "dev-libs/alt-b")
    now = egraph(system, "--layout", "human", "history", "dev-libs/alt-b")
    assert now.returncode == 0, now.stderr
    assert "dev-libs/alt-b-1 merged " in now.stdout
    assert "; no generation holds it, so now:\n" in now.stdout and "@selected" in now.stdout
    # Once a later change keeps the system it arrived in as a generation.
    shutil.rmtree(vdb(playground, "dev-libs/cond-1"))
    then = egraph(system, "--layout", "human", "history", "dev-libs/alt-b")
    assert "; then:\n" in then.stdout and "@selected" in then.stdout


def test_history_refuses_what_it_cannot_match(system):
    result = egraph(system, "history", "dev-libs/alt-b:1")
    assert result.returncode == 2
    assert "the log holds no slots" in result.stderr
