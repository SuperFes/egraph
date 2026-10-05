"""The environment the suite runs its playgrounds in."""

import os

from conftest import PORTAGE_OVERRIDES


def test_the_callers_portage_settings_never_reach_the_playgrounds():
    """CI's EMERGE_DEFAULT_OPTS and FEATURES, exported for its own emerge, once replaced the
    playgrounds' make.conf."""
    assert not set(PORTAGE_OVERRIDES) & set(os.environ)
