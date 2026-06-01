# EADS Super Node

A self-contained deployment package for the EADS monitoring system. One command starts the AnyLog master node, AnyLog query node, and the EADS super node dashboard together.

## What's Included

- **AnyLog Master Node** — manages the metadata blockchain for the sensor network
- **AnyLog Query Node** — handles data queries from the dashboard
- **Super Node** — web dashboard (port 8080) that monitors operator nodes, displays sensor data, and sends email alerts when nodes go offline

## Prerequisites

- Docker Desktop (Mac/Windows) or Docker Engine (Linux)
- An AnyLog license key
- A Gmail account with an App Password for email alerts (see below)

## Setup

**1. Clone the repo and enter the directory:**
```bash
git clone <repo-url>
cd EADS
```

**2. Create your `.env` file:**
```bash
cp .env.example .env
```

**3. Fill in the required values in `.env`** (see Configuration section below).

**4. Start everything:**
```bash
docker compose up -d --build
```

**5. Open the dashboard:** http://localhost:8080

## Configuration

All configuration lives in `.env` at the project root. This file is gitignored — never commit it.

### Required Values

| Variable | Description |
|---|---|
| `ANYLOG_LICENSE` | Your AnyLog license key |
| `EMAIL_FROM` | Gmail address to send alerts from |
| `EMAIL_TO` | Address to receive alerts (can be same as FROM) |
| `EMAIL_USERNAME` | Gmail address (same as EMAIL_FROM) |
| `EMAIL_PASSWORD` | Gmail App Password (not your regular password — see below) |

### Optional Values

| Variable | Default | Description |
|---|---|---|
| `TAG` | `ucsc-arm` | AnyLog Docker image tag |
| `OVERLAY_IP` | `10.0.0.87` | LAN IP of the machine running the containers |
| `ANYLOG_HOST` | `host.docker.internal` | Host that the super node uses to reach the query node |
| `ANYLOG_PORT` | `32449` | AnyLog query node REST port |
| `ANYLOG_DBMS` | `eads` | Logical database name to query |
| `EMAIL_SMTP_HOST` | `smtp.gmail.com` | SMTP server hostname |
| `EMAIL_SMTP_PORT` | `587` | SMTP server port |
| `MONITOR_INTERVAL_S` | `20` | How often (seconds) to check if nodes are online |
| `OFFLINE_WINDOW_S` | `120` | Seconds without data before a node is considered offline |
| `SUPERNODE_PORT` | `8080` | Port the dashboard is served on |

### Example `.env`

```dotenv
# AnyLog
INIT_TYPE=prod
TAG=ucsc-arm
ANYLOG_LICENSE=your_license_key_here
OVERLAY_IP=10.0.0.87

# Supernode
ANYLOG_HOST=host.docker.internal
ANYLOG_PORT=32449
ANYLOG_DBMS=eads
MONITOR_INTERVAL_S=20
OFFLINE_WINDOW_S=120

# Email alerts
EMAIL_SMTP_HOST=smtp.gmail.com
EMAIL_SMTP_PORT=587
EMAIL_FROM=you@gmail.com
EMAIL_TO=you@gmail.com
EMAIL_USERNAME=you@gmail.com
EMAIL_PASSWORD=xxxx xxxx xxxx xxxx
```

## Command Line Overrides

Any `.env` value can be overridden at runtime without editing the file:

```bash
# Override a single value
EMAIL_TO=someone_else@gmail.com docker compose up -d

# Override multiple values
MONITOR_INTERVAL_S=60 OFFLINE_WINDOW_S=300 docker compose up -d

# Point to a different AnyLog deployment
ANYLOG_HOST=192.168.1.100 ANYLOG_PORT=32449 docker compose up -d
```

## Setting Up a Gmail App Password

Gmail requires an App Password (not your regular password) when sending email programmatically. You need 2-Step Verification enabled on your Google account first.

**Steps:**

1. Go to your Google Account: https://myaccount.google.com
2. Click **Security** in the left sidebar
3. Under "How you sign in to Google", click **2-Step Verification** and enable it if not already on
4. Go back to Security and search for **App Passwords** (or go directly to https://myaccount.google.com/apppasswords)
5. Under "Select app", choose **Mail**
6. Under "Select device", choose **Other** and type a name like `EADS`
7. Click **Generate**
8. Copy the 16-character password shown (formatted as `xxxx xxxx xxxx xxxx`)
9. Paste it as your `EMAIL_PASSWORD` in `.env` — include the spaces

> **Note:** If you don't see App Passwords, your account may be managed by a Google Workspace admin who has disabled this feature.

## Useful Commands

```bash
# Start all services
docker compose up -d --build

# Stop all services
docker compose down

# View logs
docker logs super-node
docker logs anylog-master
docker logs anylog-query

# Attach to AnyLog CLI
docker attach --detach-keys=ctrl-d anylog-master
docker attach --detach-keys=ctrl-d anylog-query

# Rebuild just the super node after code changes
docker compose up -d --build super-node
```

## Directory Structure

```
EADS/
├── .env                          # Your local config — gitignored, never commit
├── .gitignore
├── docker-compose.yml            # Starts all three services
├── anylog/
│   ├── master-configs/
│   │   ├── base_configs.env      # AnyLog master node settings
│   │   └── advance_configs.env
│   └── query-configs/
│       ├── base_configs.env      # AnyLog query node settings
│       └── advance_configs.env
├── anomaly_notification/         # Email alert module
├── user-dashboard/               # Static dashboard files
└── super-node/
    ├── Dockerfile
    └── app/
        └── main.py               # FastAPI app
```