"""
Configuration via environment variables.

Lets each container target a different AnyLog operator + sensor identity
without touching code. Defaults match with eads_data_generator.py.
"""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- AnyLog operator target ---
    anylog_conn: str = "127.0.0.1:32149"  # host:port (or user:pass@host:port)
    anylog_mode: str = "file"  # "file" (immediate) or "streaming" (buffered)

    # --- Dataset ---
    csv_path: str = "/data/power_system_multiclass_anomaly_data.csv"

    # --- Streaming behaviour ---
    batch_size: int = 100  # rows per HTTP request
    rate_hz: float = 10.0  # batches per second (effective rows/sec = batch_size * rate_hz)

    model_config = {"env_prefix": "", "case_sensitive": False}


settings = Settings()
