# EADS Deployment Pre-Release

This folder contains the deployment-ready EADS system-wide release package. It is intended to live separately from the current `EADS-main` development repository so the stable GitHub code is not overwritten or mixed with deployment-specific files.

## Recommended repository layout

Do **not** copy these files directly over the root of `EADS-main`. The pre-release package has a different purpose and structure than the current development repo.

Recommended layout inside the repo:

```text
EADS-main/
├── README.md
├── dashboard/
├── data-generator/
├── data-ingestor/
├── fastapi-generator/
├── grafana/
├── outage-simulator/
├── super-node-module/
├── voltage-predictor/
└── deployment-pre-release/
    ├── eads-balena/
    ├── eads-master/
    └── 
```

Suggested cleanup before committing:

```bash
# From the repo root
mkdir -p deployment-pre-release
cp -R "EADS Deployment Pre-Release/eads-balena 2" deployment-pre-release/eads-balena
cp -R "EADS Deployment Pre-Release/eads-master" deployment-pre-release/eads-master
cp -R "EADS Deployment Pre-Release/eads-provisioning" deployment-pre-release/eads-provisioning
```

Then remove local-only macOS files before staging:

```bash
find deployment-pre-release -name '.DS_Store' -delete
find deployment-pre-release -name '__MACOSX' -type d -prune -exec rm -rf {} +
```

## Important security warning

This pre-release bundle may contain deployment credentials and private key material. Before pushing to GitHub, confirm whether this repository is private and whether these files should be committed at all.

Do **not** push the following to a public repository:

```text
deployment-pre-release/ca.key
deployment-pre-release/devices/*/node.key
deployment-pre-release/devices/*/eads-identity.env
deployment-pre-release/identity-pool.tar.gz
deployment-pre-release/eads-master/enroll-tls/enroll.key
deployment-pre-release/provisioned.log
```

Recommended safer approach:

```text
Commit the deployment code, compose files, scripts, and example configs.
Keep real keys, generated identities, device logs, and enrollment artifacts outside Git.
Add placeholders or `.example` files where needed.
```

A suggested `.gitignore` addition is included below.

```gitignore
# EADS deployment secrets / generated credentials
deployment-pre-release/**/ca.key
deployment-pre-release/**/node.key
deployment-pre-release/**/enroll.key
deployment-pre-release/**/eads-identity.env
deployment-pre-release/**/identity-pool.tar.gz
deployment-pre-release/**/provisioned.log
deployment-pre-release/**/devices/

# macOS archive artifacts
.DS_Store
__MACOSX/
```

## Package overview

### `eads-balena/`

Balena release package for Raspberry Pi EADS nodes. This contains the multi-container Pi deployment stack, including:

- `sampler` for real-time data sampling and ingestion
- `anylog-operator` for local AnyLog storage and synchronization
- `nebula` for encrypted overlay networking
- `gpsd` and `chrony` for GPS-backed timing
- `dashboard` for local node health visibility
- `wifi-connect` for captive-portal WiFi onboarding

Primary deployment command:

```bash
cd deployment-pre-release/eads-balena
balena push EADS
```

### `eads-master/`

Master VM deployment package. This contains the central services for the EADS deployment, including:

- AnyLog master/query services
- Nebula lighthouse configuration
- Grafana dashboards
- Fleet status and outage mapping services
- Enrollment service for plug-and-play Pi identity provisioning
- Archival support for copied node data

Typical setup flow:

```bash
cd deployment-pre-release/eads-master
cp .env.example .env
# Edit .env before starting services
./master_setup.sh
```

### ``

Provisioning kit for creating and assigning Nebula identities to Pi nodes.

Recommended plug-and-play workflow:

```bash
cd deployment-pre-release/eads-provisioning
./provision_pool.sh 2 30
scp identity-pool.tar.gz <vm>:~/eads-master/
balena env add EADS_ENROLL_TOKEN <token> --fleet EADS
```

Manual per-device workflow is also supported:

```bash
./provision_gencert.sh <N>
balena devices --fleet EADS
./provision_setenv.sh <N> <device-uuid>
```

## Deployment order

Recommended order for a clean deployment:

1. Prepare the master VM using `eads-master/`.
2. Generate or stage the identity pool using ``.
3. Set the same enrollment token in both the master `.env` and the Balena fleet variable `EADS_ENROLL_TOKEN`.
4. Push the Balena release from `eads-balena/`.
5. Flash a generic Balena fleet image to a Pi.
6. Boot the Pi and allow it to self-enroll.
7. Verify overlay and AnyLog connectivity from the master.

## Verification checklist

After the first Pi joins, verify:

```bash
balena logs <device-uuid> --service nebula
balena logs <device-uuid> --service anylog-operator
ping 10.42.0.<N>
```

Expected signs of success:

- Nebula reports the assigned overlay IP, such as `10.42.0.2`.
- AnyLog operator reports `LEDGER_CONN` pointing to `10.42.0.1:32048`.
- The master can reach the Pi over the Nebula overlay.
- The Pi appears in the fleet dashboard.
- Local dashboard is reachable at `http://<pi-lan-ip>:8080`.

## Notes for GitHub push

Use a separate branch for this pre-release:

```bash
git checkout -b deployment-pre-release
```

Stage carefully instead of blindly adding everything:

```bash
git status
git add deployment-pre-release/eads-balena deployment-pre-release/eads-master deployment-pre-release/eads-provisioning
# Then unstage/remove any secret material before committing
git status
```

Commit message example:

```bash
git commit -m "Add EADS deployment pre-release package"
```

## Current caveats

- The deployment package is intended for controlled pre-release rollout, not general public release.
- Real keys and generated node identities should be treated as secrets.
- Deploy to one Pi first and soak test before pushing fleet-wide.
- Confirm the master advertises the Nebula overlay IP and that AnyLog operators register at `10.42.0.<N>:32148` before scaling.
- GPS lock may require an outdoor or antenna-friendly install location; without GPS, chrony may depend on internet NTP.

## Provisioning Materials

Provisioning materials are intentionally excluded from this repository because they can contain private certificates, keys, identity files, and deployment-specific credentials. Keep those files outside Git and transfer them only through approved secure channels.
