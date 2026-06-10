# EADS Super Node

The Super Node serves the user dashboard and monitors node health. If a node goes offline, it sends an email alert using `anomaly_notification`.

## Run locally

1. Configure environment variables in `super-node/docker-compose.yml` or in your environment:
   - `ANYLOG_HOST` (default: `host.docker.internal`)
   - `ANYLOG_PORT` (default: `32149`)
   - `ANYLOG_DBMS` (default: `eads`)
   - `EMAIL_SMTP_HOST`
   - `EMAIL_SMTP_PORT`
   - `EMAIL_USERNAME`
   - `EMAIL_PASSWORD`
   - `EMAIL_FROM`
   - `EMAIL_TO`

2. Start the service:

```bash
cd super-node
docker compose up --build
```

3. Open the dashboard:

```text
http://localhost:8080
```

## How it works

- Serves the static dashboard from `user-dashboard/`
- Proxies `/anylog` requests to the AnyLog query node
- Runs a background offline monitor
- Sends email alerts with `anomaly_notification` when a node transitions offline
