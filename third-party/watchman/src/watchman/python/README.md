# Watchman client for Python

This directory contains the Watchman client for Python.

## Install

```sh
pip install pywatchman
```

This also installs `watchman-wait`, `watchman-make`,
`watchman-replicate-subscription` and `watchman-diag`.

## Build

```sh
python -m build
```

## Release

To cut a new release, manually bump `project.version` in `pyproject.toml` and land
the change. The next weekly ShipIt cont job exports it to GitHub, where the
`pywatchman` workflow builds and publishes the release to PyPI.
