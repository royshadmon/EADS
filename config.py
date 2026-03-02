"""
Configuration via environment variables.

Lets each container target a different AnyLog operator + sensor identity
without touching code. Defaults match the original eads_data_generator.py.
"""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- AnyLog operator target ---
    anylog_conn: str = "127.0.0.1:32149"  # host:port (or user:pass@host:port)
    anylog_dbms: str = "eads"
    anylog_table: str = "voltage_readings"

    # --- Ingestion method ---
    # "rest-put", "rest-post", or "grpc-serve"
    method: str = "rest-put"
    anylog_mode: str = "streaming"  # "streaming" or "file" (REST PUT only)
    mqtt_topic: str = "eads-sensors"  # REST POST only

    # --- Sensor identity (overrides the SENSORS list entry) ---
    sensor_id: str = "EADS-V-001"
    sensor_lat: float = 32.7157
    sensor_lon: float = -117.1611
    sensor_rms_voltage: float = 120.0

    # --- Streaming behaviour ---
    sample_rate_hz: int = 1000
    batch_size: int = 1000  # readings per HTTP request

    # --- gRPC (only used when method == "grpc-serve") ---
    grpc_port: int = 50051

    model_config = {"env_prefix": "", "case_sensitive": False}


settings = Settings()
