# EADS Deployment Pre-Release

This directory contains the deployment-focused pre-release package for EADS. It is kept separate from the main application code so the current GitHub project can remain intact while the system-wide deployment files are reviewed and tested.

## Directory Layout

```text
deployment-pre-release/
├── eads-balena/
└── eads-master/
```

## `eads-balena/`

The `eads-balena` directory contains the files used to build and deploy the Raspberry Pi / Balena-side services.

Main components include:

- `docker-compose.yml` — Balena service layout for the edge device.
- `balena.yml` — Balena project metadata.
- `sampler/` — Native sampling, anomaly detection, buffering, and ingestion code.
- `dashboard/` — Lightweight local dashboard service.
- `gpsd/` — GPS service startup and fix publishing scripts.
- `chrony/` — Time synchronization service configuration.
- `nebula/` — Nebula container startup files for device networking.
- `wifi-connect/` — Wi-Fi connection support for device setup.
- `anylog-operator/` — AnyLog operator deployment script and container files.
- `NEBULA_SETUP.md` — Notes for Nebula-related setup.
- `README.md` — Balena-specific notes.

This folder is intended for edge-node deployment testing and packaging.

## `eads-master/`

The `eads-master` directory contains the files used on the master / server-side deployment.

Main components include:

- `docker-compose.yml` — Master-side service layout.
- `master_setup.sh` — Setup script for the master deployment.
- `archiver/` — Archiver service and Python requirements.
- `fleet/` — Fleet management service, API code, and example registry.
- `grafana/dashboards/` — Grafana dashboard JSON files for AnyLog and fleet visibility.
- `anylog/` — AnyLog support script for duplicate policy cleanup.
- `nebula/` — Nebula lighthouse configuration and service file.
- `identity-pool/.gitkeep` — Placeholder directory for runtime identity material.

This folder is intended for the master node that coordinates, observes, and supports deployed EADS devices.

## Excluded Files

Provisioning materials are intentionally not included in this directory.

The following types of files should not be committed to Git:

- private keys
- certificates
- `.env` files
- device identity files
- generated device folders
- identity pool archives
- provisioning logs
- provisioning scripts or folders that contain secrets

These files should be generated or transferred through a secure deployment process instead of being stored in the repository.

## Deployment Notes

This pre-release is meant to be reviewed before being treated as a final production deployment package.

Before using it in the field:

1. Review the Balena service definitions in `eads-balena/docker-compose.yml`.
2. Review the master service definitions in `eads-master/docker-compose.yml`.
3. Confirm that all runtime secrets and identities are provided outside of Git.
4. Confirm that Nebula configuration files match the target deployment network.
5. Test the master services and edge services separately before system-wide deployment.

## Current Status

This is a pre-release deployment package. It is organized for review, testing, and integration with the main EADS repository.
