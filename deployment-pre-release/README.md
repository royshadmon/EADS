# EADS Deployment Pre-Release

This directory contains the deployment-focused pre-release package for EADS. It is kept separate from the main application code so the deployment files can be reviewed and tested without disrupting the current project structure.

## Directory Layout

- `eads-balena/`: edge-device deployment files for Raspberry Pi / Balena services.
- `eads-master/`: master-node deployment files for server-side services.

## eads-balena

The `eads-balena` folder contains the Balena-side services used on deployed edge devices.

Main contents:

- `docker-compose.yml`: Balena service layout for the edge device.
- `balena.yml`: Balena project metadata.
- `sampler/`: sampling, buffering, anomaly detection, and ingestion code.
- `dashboard/`: lightweight local dashboard service.
- `gpsd/`: GPS service startup and fix publishing scripts.
- `chrony/`: time synchronization service configuration.
- `nebula/`: Nebula container startup files for device networking.
- `wifi-connect/`: Wi-Fi setup support for device onboarding.
- `anylog-operator/`: AnyLog operator deployment files.
- `NEBULA_SETUP.md`: Nebula setup notes.

## eads-master

The `eads-master` folder contains the master/server-side deployment files.

Main contents:

- `docker-compose.yml`: master-side service layout.
- `master_setup.sh`: setup script for the master deployment.
- `archiver/`: archiver service and requirements.
- `fleet/`: fleet management API and example registry.
- `grafana/dashboards/`: Grafana dashboard JSON files.
- `anylog/`: AnyLog support scripts.
- `nebula/`: Nebula lighthouse configuration and service file.
- `identity-pool/.gitkeep`: placeholder for runtime identity material.

## Excluded Files

Provisioning materials are intentionally not included in this directory.

The following files should not be committed to Git:

- private keys
- certificates
- `.env` files
- device identity files
- generated device folders
- identity pool archives
- provisioning logs
- provisioning folders that contain secrets

These files should be generated or transferred through a secure deployment process instead of being stored in the repository.

## Deployment Notes

Before field use, review the Balena and master Docker Compose files, confirm runtime secrets are provided outside Git, verify Nebula configuration, and test edge and master services separately.

## Current Status

This is a pre-release deployment package organized for review, testing, and integration with the main EADS repository.
