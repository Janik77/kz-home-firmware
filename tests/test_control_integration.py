"""Pipe transport: actual Core authority/ledger and production firmware C.

Only time qualification, GPIO/NVS/SDK boundaries are lab doubles. No broker or
installation is accessed; Core source is read-only and all DBs are temporary.
"""
import argparse
from datetime import UTC, datetime, timedelta
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from uuid import uuid4

parser = argparse.ArgumentParser()
parser.add_argument("--core-root", type=Path, required=True)
parser.add_argument("--bridge", type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.core_root.resolve()))

from sqlalchemy import create_engine, event
from sqlalchemy.orm import Session
from app.db import Base
from app.models import (HouseORM, FloorORM, RoomORM, UserORM, DeviceORM,
                        PhysicalDeviceORM, DeviceCommandORM)
from app.repositories.command_repository import CommandRepository
from app.services.command_service import CommandService
from app.services.control_authority import ControlAuthority
from app.services.control_clock import ControlClock
from app.transports.control_models import decode_request
from app.transports.mqtt_models import AckEnvelope, StateEnvelope
from app.core.errors import ConflictError


def check(mode):
    with tempfile.TemporaryDirectory(prefix="kz-control-integration-") as directory:
        engine = create_engine("sqlite:///" + str(Path(directory) / "fixture.db"))
        @event.listens_for(engine, "connect")
        def foreign_keys(connection, _):
            connection.execute("PRAGMA foreign_keys=ON")
        Base.metadata.create_all(engine)
        tick = [0]
        clock = ControlClock(ticks=lambda: tick[0], qualified=True,
            utc_health=lambda: True,
            wall=lambda: datetime(2026, 11, 2, 10, tzinfo=UTC)+timedelta(microseconds=tick[0]))
        authority = ControlAuthority(clock, gate=lambda *unused: None)
        with Session(engine) as s:
            s.add(HouseORM(id="test_house", name="Lab")); s.flush()
            s.add(FloorORM(id="floor", house_id="test_house", name="Lab", order=0)); s.flush()
            s.add(RoomORM(id="room", floor_id="floor", name="Lab")); s.flush()
            s.add(UserORM(id="actor", email="lab@example.test", password_hash="unused")); s.flush()
            s.add(DeviceORM(id="test_relay", room_id="room", name="Lab", type="relay",
                            capabilities=["on_off"], state={}, online=True, metadata_={})); s.flush()
            s.add(PhysicalDeviceORM(device_id="test_relay", hardware_id="lab-serial",
                hardware_model="esp32-relay-v1", protocol_version="v1", house_id="test_house",
                bound_device_id="test_relay", status="active", claim_code_hash=None)); s.commit()
            authority.initialize(s)
        tick[0] += clock.fence_us
        now, _ = clock.sample()
        identifier, correlation = str(uuid4()), str(uuid4())
        with Session(engine) as s:
            authority.prepare(s, DeviceCommandORM(command_id=identifier, correlation_id=correlation,
                house_id="test_house", device_id="test_relay", requested_by="actor",
                idempotency_hash="a"*64, requested_on=True, status="queued",
                created_at=now, updated_at=now, expires_at=now+timedelta(seconds=30)))
            delivery = authority.stage(s, "test_house", "test_relay", identifier)
        child = subprocess.Popen([str(args.bridge), "late" if mode == "late-reply" else "normal"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            child.stdin.write(delivery.payload+"\n"); child.stdin.flush()
            request = decode_request(child.stdout.readline().encode())
            if mode == "expired":
                tick[0] += 30_000_000
            if mode == "bad-digest":
                request = request.model_copy(update={"intent_sha256": "0"*64})
            with Session(engine) as s:
                try:
                    reply = authority.request(s, "test_house", "test_relay", request).payload
                    assert mode not in ("expired", "bad-digest")
                except ConflictError:
                    assert mode in ("expired", "bad-digest")
                    reply = json.dumps({**request.model_dump(), "type":"permit_denied", "reason":"unavailable"})
            child.stdin.write(reply+"\n"); child.stdin.flush()
            result = json.loads(child.stdout.readline())
            assert child.wait(timeout=10) == 0
            assert result["writes"] == (1 if mode in ("apply", "late-ack") else 0), result
            with Session(engine) as s:
                service = CommandService(CommandRepository(s), control_authority=authority)
                if mode in ("apply", "late-ack"):
                    assert result["status"] == "applied" and result["on"]
                    if mode == "late-ack":
                        tick[0] += 30_000_000
                    service.ack("test_house", "test_relay", AckEnvelope(command_id=identifier,
                                correlation_id=correlation, status=result["status"]))
                    service.state("test_house", "test_relay", StateEnvelope(timestamp=now,
                                  correlation_id=correlation, state={"on":result["on"]}))
                    expected = "timed_out" if mode == "late-ack" else "applied"
                    assert service.get("test_house", identifier).status == expected
                    if mode == "late-ack":
                        row = s.get(DeviceCommandORM, identifier)
                        assert row.ack_status is None and row.state_received_at is None
                    tick[0] += 30_000_000
                    assert service.get("test_house", identifier).status == expected
                elif mode == "expired":
                    assert service.get("test_house", identifier).status == "timed_out"
        finally:
            if child.poll() is None:
                child.kill(); child.wait()
            child.stdin.close(); child.stdout.close(); engine.dispose()


for scenario in ("apply", "expired", "bad-digest", "late-reply", "late-ack"):
    check(scenario)
print("PASS: 5 actual Core authority -> firmware C scenarios (apply, expiry, digest, delayed reply/ACK)")
