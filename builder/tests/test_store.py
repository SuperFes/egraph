import os
import stat

import pytest

from egraph_build import cli, installed, store
from egraph_build.profile import ImplicitIuse

META = store.Meta(
    "0.0.0",
    "3.0.0",
    "/tmp/eroot/",
    1_700_000_000_123_456_789,
    ImplicitIuse(("amd64", "elibc_glibc"), ("build", "x86"), ("elibc_", "kernel_")),
)
INPUTS = (
    store.Input(
        "/var/db/pkg/app-misc", store.INPUT_DIRECTORY, 1_700_000_000_000_000_001, 0
    ),
    store.Input("/var/lib/portage/world", store.INPUT_FILE, 2**63, 1234),
    store.Input("/odd/\udcff-byte", store.INPUT_FILE, 0, 0),
)


def test_round_trip(scenario):
    layer = installed.build(scenario.vardb)
    meta, inputs, decoded = store.decode(store.encode(layer, META, INPUTS))
    assert (meta, inputs) == (META, INPUTS)
    assert installed.to_json(decoded) == installed.to_json(layer)


def test_encoding_is_deterministic(playgrounds):
    vardb = playgrounds("reference").vardb
    first = store.encode(installed.build(vardb), META, INPUTS)
    assert first == store.encode(installed.build(vardb), META, INPUTS)


@pytest.fixture(scope="module")
def reference_store(playgrounds):
    return store.encode(installed.build(playgrounds("reference").vardb), META, INPUTS)


def test_every_truncation_is_rejected(reference_store):
    for size in range(len(reference_store)):
        with pytest.raises(store.StoreError):
            store.decode(reference_store[:size])


@pytest.mark.parametrize(
    "offset, value, message",
    [
        (0, ord("X"), "not an egraph store"),
        (8, 4, "format version 4"),
        (12, 4, "bad section table"),
    ],
)
def test_header_is_checked(reference_store, offset, value, message):
    data = bytearray(reference_store)
    data[offset] = value
    with pytest.raises(store.StoreError, match=message):
        store.decode(bytes(data))


def test_trailing_bytes_in_a_section_are_rejected(reference_store):
    # Grow the last section, profile, by one byte.
    data = bytearray(reference_store + b"\0")
    entry = store._HEADER.size + store._ENTRY.size * 5
    section_id, offset, length = store._ENTRY.unpack_from(data, entry)
    assert section_id == store.SECTION_PROFILE
    store._ENTRY.pack_into(data, entry, section_id, offset, length + 1)
    with pytest.raises(store.StoreError, match="profile: trailing bytes"):
        store.decode(bytes(data))


def test_write_replaces_atomically(tmp_path):
    path = tmp_path / "cache" / "installed.egraph"
    store.write(path, b"old")
    store.write(path, b"new")
    assert path.read_bytes() == b"new"
    assert stat.S_IMODE(path.stat().st_mode) == 0o644
    assert os.listdir(path.parent) == ["installed.egraph"]


def test_full_writes_the_store(monkeypatch, tmp_path, playgrounds):
    vardb = playgrounds("reference").vardb
    monkeypatch.setattr(cli, "open_vardb", lambda *args: vardb)
    path = tmp_path / "x.egraph"
    assert cli.main(["--full", "--store", str(path)]) == cli.EXIT_OK
    meta, inputs, layer = store.decode(path.read_bytes())
    assert meta.eroot == vardb.settings["EROOT"]
    assert installed.to_json(layer) == installed.to_json(installed.build(vardb))


def test_full_defaults_to_the_eroots_cache(monkeypatch, playgrounds):
    vardb = playgrounds("reference").vardb
    monkeypatch.setattr(cli, "open_vardb", lambda *args: vardb)
    monkeypatch.delenv("EGRAPH_STORE", raising=False)
    path = store.default_path(vardb.settings["EROOT"])
    assert cli.main(["--full"]) == cli.EXIT_OK
    assert os.path.isfile(path)
    os.unlink(path)


def test_full_records_the_profiles_implicit_iuse(monkeypatch, tmp_path, playgrounds):
    from egraph_build import profile

    vardb = playgrounds("reference").vardb
    monkeypatch.setattr(cli, "open_vardb", lambda *args: vardb)
    path = tmp_path / "x.egraph"
    assert cli.main(["--full", "--store", str(path)]) == cli.EXIT_OK
    meta, _, _ = store.decode(path.read_bytes())
    assert meta.implicit == profile.implicit_iuse(vardb.settings)
    # bootstrap.sh's flags are always implied for EAPIs before 5.
    assert {"build", "bootstrap"} <= set(meta.implicit.literals)
