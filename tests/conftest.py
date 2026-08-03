"""Shared fixtures for the offline-tooling unit tests (issue #24).

tools/ holds standalone scripts, not an importable package — there's no
__init__.py and the filenames aren't valid module paths to import from a test
that lives elsewhere. Loading them through importlib keeps them runnable as
plain `python3 tools/foo.py` scripts (which is how build.sh invokes them)
while still letting the tests reach their individual functions.
"""

import importlib.util
import pathlib
import shutil
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_ROOT / "tools"


def _load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="session")
def sign_rom_ticket():
    return _load("sign_rom_ticket", TOOLS_DIR / "sign_rom_ticket.py")


@pytest.fixture(scope="session")
def generate_signing_key():
    return _load("generate_signing_key", TOOLS_DIR / "generate_signing_key.py")


@pytest.fixture(scope="session")
def openssl_required():
    """Both tools shell out to openssl; skip rather than fail if it's absent."""
    if shutil.which("openssl") is None:
        pytest.skip("openssl not on PATH")


@pytest.fixture(scope="session")
def signing_key(tmp_path_factory, generate_signing_key, openssl_required):
    """A throwaway RSA-2048 keypair, generated once for the whole session.

    Explicitly writes both outputs into a tmp dir. The default paths point at
    tools/keys/update_signing_key.pem and the *tracked*
    OrreryPkg/Include/TrustedUpdateKey.h — a test run must never touch either,
    or it would silently rewrite the real compiled-in signing key.
    """
    out = tmp_path_factory.mktemp("signing-key")
    key = out / "test_key.pem"
    header = out / "TrustedUpdateKey.h"
    generate_signing_key.main(["--key-out", str(key), "--header-out", str(header)])
    return key, header
