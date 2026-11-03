"""Opt-in disposable Docker PostgreSQL/Mosquitto TLS + real Core/C host adapter.

Never reads .env, production URLs, local device headers or deployed volumes.
All certificates/passwords are ephemeral. Positive control is lab injection only.
"""

import argparse
import asyncio
from concurrent.futures import ThreadPoolExecutor
from datetime import UTC, datetime, timedelta
import json
import os
from pathlib import Path
import secrets
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
from uuid import uuid4


def docker(*args, check=True, **kwargs):
    result = subprocess.run(
        ["docker", *args], capture_output=True, text=True, timeout=120, **kwargs
    )
    if check and result.returncode:
        raise RuntimeError(
            "Disposable Docker operation failed: " + result.stderr[-1500:]
        )
    return result.stdout.strip()


class Services:
    def __init__(self):
        self.name = "kz-control-lab-" + uuid4().hex[:12]
        self.directory = Path(tempfile.mkdtemp(prefix=self.name + "-"))
        self.password = secrets.token_urlsafe(32)
        self.containers = []
        self.network = None

    def start(self):
        self.network = docker(
            "network", "create", "--label", "kz.control.lab=" + self.name, self.name
        )
        root = self.directory.as_posix()
        # Offline certificate generation: SAN deliberately DNS localhost only.
        certs = """
set -eu
cd /lab
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 1 -subj /CN=KZ-Disposable-Test-CA >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj /CN=localhost >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n' > extensions
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 1 -extfile extensions >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -keyout wrong.key -out wrong.crt -days 1 -subj /CN=Wrong-Test-CA >/dev/null 2>&1
chmod 644 server.key
"""
        docker(
            "run",
            "--rm",
            "--network",
            "none",
            "--mount",
            f"type=bind,source={root},target=/lab",
            "--entrypoint",
            "sh",
            "alpine/openssl:latest",
            "-c",
            certs,
        )
        users = ("core_lab", "physical_lab", "other_lab", "software_lab")
        (self.directory / "passwd").write_text(
            "\n".join(f"{user}:{self.password}" for user in users) + "\n",
            encoding="utf-8",
        )
        docker(
            "run",
            "--rm",
            "--network",
            "none",
            "--mount",
            f"type=bind,source={root},target=/lab",
            "--entrypoint",
            "mosquitto_passwd",
            "eclipse-mosquitto:2",
            "-U",
            "/lab/passwd",
        )
        prefix = "kzhome/v1/test_house/test_relay"
        # Server rules mirror tracked Core ACL; physical rights are exact topics.
        acl = (
            "user core_lab\n"
            + "\n".join(
                [
                    "topic read kzhome/v1/+/+/control/request",
                    "topic write kzhome/v1/+/+/control/command",
                    "topic write kzhome/v1/+/+/control/reply",
                    "topic read kzhome/v1/+/+/ack",
                    "topic read kzhome/v1/+/+/state",
                    "topic read kzhome/v1/+/+/status",
                    "topic read kzhome/v1/+/+/telemetry",
                    "topic write kzhome/v1/test_house/software_relay/set",
                    # Deliberately no physical /set permission, even for this Core fixture.
                    "user physical_lab",
                    f"topic read {prefix}/control/command",
                    f"topic read {prefix}/control/reply",
                    f"topic write {prefix}/control/request",
                    f"topic write {prefix}/ack",
                    f"topic write {prefix}/state",
                    f"topic write {prefix}/status",
                    "user other_lab",
                    "topic readwrite kzhome/v1/other_house/other_relay/#",
                    "user software_lab",
                    "topic read kzhome/v1/test_house/software_relay/set",
                    "topic write kzhome/v1/test_house/software_relay/ack",
                    "topic write kzhome/v1/test_house/software_relay/state",
                ]
            )
            + "\n"
        )
        (self.directory / "acl").write_text(acl, encoding="utf-8")
        (self.directory / "mosquitto.conf").write_text(
            """listener 8883
allow_anonymous false
password_file /lab/passwd
acl_file /lab/acl
cafile /lab/ca.crt
certfile /lab/server.crt
keyfile /lab/server.key
tls_version tlsv1.2
persistence false
log_dest stdout
log_type all
""",
            encoding="utf-8",
        )
        pg = self.name + "-pg"
        self.containers.append(pg)
        docker(
            "run",
            "-d",
            "--name",
            pg,
            "--label",
            "kz.control.lab=" + self.name,
            "--network",
            self.network,
            "--tmpfs",
            "/var/lib/postgresql/data:rw",
            "-e",
            "POSTGRES_USER=lab",
            "-e",
            "POSTGRES_DB=lab",
            "-e",
            "POSTGRES_PASSWORD=" + self.password,
            "-p",
            "127.0.0.1::5432",
            "postgres:17-bookworm",
        )
        mqtt = self.name + "-mqtt"
        self.containers.append(mqtt)
        docker(
            "run",
            "-d",
            "--name",
            mqtt,
            "--label",
            "kz.control.lab=" + self.name,
            "--network",
            self.network,
            "--mount",
            f"type=bind,source={root},target=/lab,readonly",
            "--tmpfs",
            "/mosquitto/data",
            "--tmpfs",
            "/mosquitto/log",
            "-p",
            "127.0.0.1::8883",
            "eclipse-mosquitto:2",
            "mosquitto",
            "-c",
            "/lab/mosquitto.conf",
        )
        for _ in range(60):
            if "accepting connections" in docker(
                "exec", pg, "pg_isready", "-U", "lab", check=False
            ):
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("Temporary PostgreSQL did not become ready")
        self.pg_port = int(docker("port", pg, "5432/tcp").rsplit(":", 1)[1])
        self.mqtt_port = int(docker("port", mqtt, "8883/tcp").rsplit(":", 1)[1])
        self.url = (
            f"postgresql+psycopg://lab:{self.password}@127.0.0.1:{self.pg_port}/lab"
        )
        print(
            "SERVICES: PostgreSQL 17 + Mosquitto TLS, random loopback ports, fresh tmpfs data",
            flush=True,
        )
        # Import supplied Core ACL text to verify the server permissions used above.
        return self

    def close(self):
        for name in reversed(self.containers):
            label = docker(
                "inspect",
                "--format",
                '{{index .Config.Labels "kz.control.lab"}}',
                name,
                check=False,
            )
            if label == self.name:
                docker("rm", "-f", "-v", name)
            elif label:
                raise RuntimeError("Refusing cleanup of foreign container")
        if self.network:
            label = docker(
                "network",
                "inspect",
                "--format",
                '{{index .Labels "kz.control.lab"}}',
                self.network,
            )
            if label != self.name:
                raise RuntimeError("Refusing cleanup of foreign network")
            docker("network", "rm", self.network)
        target = self.directory.resolve()
        if target.parent != Path(
            tempfile.gettempdir()
        ).resolve() or not target.name.startswith(self.name + "-"):
            raise RuntimeError(
                "Refusing cleanup outside the allocated temporary directory"
            )
        shutil.rmtree(target)
        print(
            "CLEANUP: own containers/tmpfs/network/certificates/password files removed",
            flush=True,
        )


class Firmware:
    def __init__(self, bridge, lab=True):
        self.child = subprocess.Popen(
            [str(bridge), *(["lab"] if lab else [])],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        self.last = time.monotonic()

    def op(self, kind, payload=None):
        # Real elapsed host time is charged to the fake SDK timer before each op.
        elapsed = int((time.monotonic() - self.last) * 1000)
        self.last = time.monotonic()
        if elapsed > 0:
            self._line("advance", str(elapsed))
        return self._line(kind, payload)

    def _line(self, kind, payload):
        self.child.stdin.write(
            kind + ("\t" + payload if payload is not None else "") + "\n"
        )
        self.child.stdin.flush()
        line = self.child.stdout.readline()
        assert line, "Firmware host process exited"
        return json.loads(line)

    def close(self):
        self.child.stdin.close()
        try:
            assert self.child.wait(timeout=5) == 0
        finally:
            if self.child.poll() is None:
                self.child.kill()
                self.child.wait()
            self.child.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-root", type=Path, required=True)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument(
        "--full-core",
        action="store_true",
        help="Run full Core pytest against the temporary PostgreSQL",
    )
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.core_root.resolve()))
    import aiomqtt
    from alembic import command
    from alembic.config import Config
    from alembic.autogenerate import compare_metadata
    from alembic.migration import MigrationContext
    from sqlalchemy import create_engine, select, text
    from sqlalchemy.exc import IntegrityError
    from sqlalchemy.orm import Session
    from app.db import Base
    from app.models import (
        HouseORM,
        FloorORM,
        RoomORM,
        UserORM,
        DeviceORM,
        PhysicalDeviceORM,
        DeviceCommandORM,
        ControlCommandORM,
    )
    from app.events import EventBus
    from app.core.errors import ConflictError, UnavailableError
    from app.repositories.command_repository import CommandRepository
    from app.repositories.physical_device_repository import PhysicalDeviceRepository
    from app.repositories.device_repository import DeviceRepository
    from app.services.onboarding_service import PhysicalBindingGuard
    from app.services.command_service import CommandService
    from app.services.control_authority import ControlAuthority
    from app.services.control_clock import ControlClock
    from app.transports.mqtt import MQTTGateway
    from app.transports.mqtt_client import AiomqttClient
    from app.transports.control_models import decode_request

    services = Services()
    engine = None
    try:
        services.start()
        engine = create_engine(services.url)

        def config(connection):
            c = Config()
            c.set_main_option("script_location", str(args.core_root / "alembic"))
            c.attributes["connection"] = connection
            return c

        def row(identifier=None):
            now, _ = authority.clock.sample()
            return DeviceCommandORM(
                command_id=identifier or str(uuid4()),
                correlation_id=str(uuid4()),
                house_id="test_house",
                device_id="test_relay",
                requested_by="actor",
                idempotency_hash=uuid4().hex * 2,
                requested_on=True,
                status="queued",
                created_at=now,
                updated_at=now,
                expires_at=now + timedelta(seconds=30),
            )

        # Use a controllable clock only inside the isolated integration fixture.
        class Time:
            def __init__(self):
                self.offset = 0
                self.wall_offset = 0
                self.health = True

            def ticks(self):
                return time.monotonic_ns() // 1000 + self.offset

            def wall(self):
                return datetime.now(UTC) + timedelta(
                    microseconds=self.offset + self.wall_offset
                )

            def clock(self, qualified=True):
                return ControlClock(
                    ticks=self.ticks,
                    wall=self.wall,
                    qualified=qualified,
                    utc_health=lambda: self.health,
                )

        timer = Time()
        authority = ControlAuthority(timer.clock(), gate=lambda *unused: None)
        with engine.begin() as c:
            command.upgrade(config(c), "0006_command_lifecycle")
        with Session(engine) as s:
            s.add(HouseORM(id="test_house", name="Lab"))
            s.flush()
            s.add(FloorORM(id="floor", house_id="test_house", name="Lab", order=0))
            s.flush()
            s.add(RoomORM(id="room", floor_id="floor", name="Lab"))
            s.flush()
            s.add(
                UserORM(
                    id="actor", email="actor@example.test", password_hash="not-a-login"
                )
            )
            s.flush()
            s.add(
                DeviceORM(
                    id="test_relay",
                    room_id="room",
                    name="Lab",
                    type="relay",
                    capabilities=["on_off"],
                    state={},
                    online=True,
                    metadata_={},
                )
            )
            s.flush()
            s.add(
                DeviceORM(
                    id="software_relay",
                    room_id="room",
                    name="Software",
                    type="relay",
                    capabilities=["on_off"],
                    state={},
                    online=True,
                    metadata_={},
                )
            )
            s.flush()
            s.add(
                PhysicalDeviceORM(
                    device_id="test_relay",
                    hardware_id="lab-only-serial",
                    hardware_model="esp32-relay-v1",
                    protocol_version="v1",
                    house_id="test_house",
                    bound_device_id="test_relay",
                    status="active",
                    claim_code_hash=None,
                )
            )
            now = datetime.now(UTC)
            s.add(
                DeviceCommandORM(
                    command_id=str(uuid4()),
                    correlation_id=str(uuid4()),
                    house_id="test_house",
                    device_id="test_relay",
                    requested_by="actor",
                    idempotency_hash="a" * 64,
                    requested_on=True,
                    status="timed_out",
                    created_at=now - timedelta(seconds=60),
                    updated_at=now,
                    expires_at=now - timedelta(seconds=30),
                )
            )
            s.commit()
        with engine.begin() as c:
            before = c.execute(text("SELECT * FROM device_commands")).mappings().all()
            command.upgrade(config(c), "head")
            assert (
                c.execute(text("SELECT * FROM device_commands")).mappings().all()
                == before
            )
            differences = compare_metadata(MigrationContext.configure(c), Base.metadata)
            # Historical 0003 creates both email UNIQUE + unique index; ORM keeps
            # the index. PostgreSQL exposes the redundant constraint separately.
            assert all(
                d[0] == "remove_constraint"
                and d[1].name == "uq_users_email"
                and d[1].table.name == "users"
                for d in differences
            ), differences
            if differences:
                print(
                    "NOTE: pre-existing 0003 redundant users.email UNIQUE differs from ORM; unchanged",
                    flush=True,
                )
            command.downgrade(config(c), "0006_command_lifecycle")
            command.upgrade(config(c), "head")
            assert (
                c.execute(text("SELECT * FROM device_commands")).mappings().all()
                == before
            )
        with Session(engine) as s:
            authority.initialize(s)
        timer.offset += authority.clock.fence_us
        print(
            "POSTGRES: 0006->0007 preserved ledger, control metadata aligned, empty downgrade/upgrade PASS",
            flush=True,
        )

        def prepare(lifetime_us=30_000_000):
            timer.offset += 31_000_000
            with Session(engine) as s:
                service = CommandService(
                    CommandRepository(s), control_authority=authority
                )
                for pending in s.scalars(
                    select(DeviceCommandORM).where(
                        DeviceCommandORM.status.in_(("queued", "dispatched"))
                    )
                ):
                    service.expire(pending)
                s.commit()
                value = row()
                value.expires_at = value.created_at + timedelta(
                    microseconds=lifetime_us
                )
                identifier = value.command_id
                authority.prepare(s, value)
                return identifier

        identifier = prepare()
        # Real constraints + atomic parent/child failure using an invalid digest.
        for changes in (
            {"wire_sha256": "bad"},
            {"execution_profile": "unknown"},
            {"authority_epoch": "bad"},
            {"wire_payload": "x" * 4097},
        ):
            with Session(engine) as s:
                record = s.get(ControlCommandORM, identifier)
                for key, value in changes.items():
                    setattr(record, key, value)
                try:
                    s.commit()
                except IntegrityError:
                    s.rollback()
                else:
                    raise AssertionError("PostgreSQL accepted invalid control metadata")
        import app.services.control_authority as authority_module

        original_digest = authority_module.digest
        timer.offset += 31_000_000
        with Session(engine) as s:
            pending = s.get(DeviceCommandORM, identifier)
            CommandService(CommandRepository(s), control_authority=authority).expire(
                pending
            )
            s.commit()
        with Session(engine) as s:
            broken = row()
            broken_id = broken.command_id
            authority_module.digest = lambda _: "invalid"
            try:
                try:
                    authority.prepare(s, broken)
                except ConflictError:
                    pass
                else:
                    raise AssertionError("Invalid child insert succeeded")
            finally:
                authority_module.digest = original_digest
        with Session(engine) as s:
            assert s.get(DeviceCommandORM, broken_id) is None
            assert s.get(ControlCommandORM, broken_id) is None
        with engine.connect() as c:
            try:
                command.downgrade(config(c), "0006_command_lifecycle")
            except RuntimeError:
                c.rollback()
            else:
                raise AssertionError("Populated control downgrade succeeded")
        with Session(engine) as s:
            s.add(
                ControlCommandORM(
                    command_id=str(uuid4()),
                    execution_profile="deadline-permit-v1",
                    authority_epoch=str(uuid4()),
                    wire_payload="{}",
                    wire_sha256="a" * 64,
                )
            )
            try:
                s.commit()
            except IntegrityError:
                s.rollback()
            else:
                raise AssertionError("Orphan control FK accepted")
            s.delete(s.get(DeviceCommandORM, identifier))
            try:
                s.commit()
            except IntegrityError:
                s.rollback()
            else:
                raise AssertionError("Ledger RESTRICT FK did not protect history")
        print(
            "POSTGRES: CHECK constraints, transactional rollback, populated downgrade guard PASS",
            flush=True,
        )

        def client(
            user="physical_lab", *, hostname="localhost", ca="ca.crt", password=None
        ):
            ctx = ssl.create_default_context(cafile=str(services.directory / ca))
            return aiomqtt.Client(
                hostname,
                port=services.mqtt_port,
                username=user,
                password=password if password is not None else services.password,
                tls_context=ctx,
                identifier="lab-" + uuid4().hex,
                timeout=5,
                protocol=aiomqtt.ProtocolVersion.V5,
            )

        async def receive(c, topic, timeout=5):
            async with asyncio.timeout(timeout):
                async for m in c.messages:
                    if str(m.topic) == topic:
                        return bytes(m.payload).decode()
            raise AssertionError("No message")

        async def empty(c, topic):
            try:
                await receive(c, topic, 0.3)
            except TimeoutError:
                return
            raise AssertionError("Forbidden MQTT publication was delivered")

        async def test_network():
            nonlocal authority
            base = "kzhome/v1/test_house/test_relay"
            for kwargs in (
                {"ca": "wrong.crt"},
                {"hostname": "127.0.0.1"},
                {"password": "wrong-test-password"},
            ):
                try:
                    async with client(**kwargs):
                        pass
                except (aiomqtt.MqttError, ssl.SSLError):
                    pass
                else:
                    raise AssertionError("Invalid TLS/auth accepted")
            # Anonymous must fail even with the trusted test CA.
            ctx = ssl.create_default_context(cafile=str(services.directory / "ca.crt"))
            try:
                async with aiomqtt.Client(
                    "localhost", port=services.mqtt_port, tls_context=ctx, timeout=5
                ):
                    pass
            except aiomqtt.MqttError:
                pass
            else:
                raise AssertionError("Anonymous MQTT accepted")
            async with client("core_lab") as server, client() as device:
                await device.subscribe(base + "/control/command", qos=1)
                await device.subscribe(base + "/control/reply", qos=1)
                for topic in (base + "/control/reply", base + "/control/command"):
                    # MQTT v5 returns Not authorized; absence of delivery also checked.
                    try:
                        await device.publish(topic, "forbidden", qos=1)
                    except aiomqtt.MqttError:
                        pass
                    await empty(device, topic)
                foreign = "kzhome/v1/other_house/other_relay/control/command"
                async with client("other_lab") as other:
                    await other.subscribe(foreign, qos=1)
                    try:
                        await device.publish(foreign, "forbidden", qos=1)
                    except aiomqtt.MqttError:
                        pass
                    await empty(other, foreign)
                    try:
                        await device.subscribe(foreign, qos=1)
                    except aiomqtt.MqttError:
                        pass
                    await other.publish(foreign, "foreign", qos=1)
                    await empty(device, foreign)
                # Legacy physical /set has no ACL permission, even for test Core.
                try:
                    await server.publish(base + "/set", "{}", qos=1)
                except aiomqtt.MqttError:
                    pass
            print(
                "MQTT REAL: trusted TLS/auth PASS; wrong CA/SAN/password/anonymous denied; directional/foreign ACL PASS",
                flush=True,
            )

            wire = AiomqttClient(
                host="localhost",
                port=services.mqtt_port,
                username="core_lab",
                password=services.password,
                tls_enabled=True,
                keepalive=15,
                client_id="lab-gateway-" + uuid4().hex,
            )
            wire._options["tls_context"] = ctx
            evidence = []

            async def identity(house, device):
                with Session(engine) as s:
                    assert DeviceRepository(s).house_id(device) == house
                    if device != "software_relay":
                        PhysicalDeviceRepository(s).for_house(house, device)

            async def noop(*unused):
                return True

            async def reported(h, d, patch, correlation):
                with Session(engine) as s:
                    device = s.get(DeviceORM, d)
                    device.state = patch
                    s.commit()

            async def guard(h, d):
                await identity(h, d)
                with Session(engine) as s:
                    PhysicalBindingGuard(
                        PhysicalDeviceRepository(s)
                    ).require_command_contract(d)

            async def stage(h, d, i):
                with Session(engine) as s:
                    return authority.stage(s, h, d, i)

            async def request(h, d, value):
                with Session(engine) as s:
                    return authority.request(s, h, d, value)

            async def ack(h, d, value, retained):
                with Session(engine) as s:
                    CommandService(
                        CommandRepository(s), control_authority=authority
                    ).ack(h, d, value, retained)
                evidence.append(("ack", value.command_id))

            async def state(h, d, value, retained):
                with Session(engine) as s:
                    CommandService(
                        CommandRepository(s), control_authority=authority
                    ).state(h, d, value, retained)
                evidence.append(("state", value.correlation_id))

            gateway = MQTTGateway(
                wire,
                EventBus(),
                reported,
                noop,
                identity,
                command_guard=guard,
                command_ack_handler=ack,
                command_state_handler=state,
                control_authority=authority,
                control_stage_handler=stage,
                control_request_handler=request,
            )
            gateway.start()

            async def ready():
                async with asyncio.timeout(10):
                    while not gateway.connected or not gateway.control_subscribed:
                        await asyncio.sleep(0.02)

            await ready()
            try:
                for mode in (
                    "apply",
                    "blocked",
                    "expired",
                    "delayed-command",
                    "delayed-reply",
                    "duplicate-request",
                    "replay",
                    "reconnect",
                    "reboot",
                    "clock-fault",
                    "restart",
                    "late-ack",
                    "bad-digest",
                ):
                    # Transport limiter is real; space requests rather than bypass it.
                    await asyncio.sleep(1.05)
                    identifier = prepare(
                        600_000 if mode == "delayed-command" else 30_000_000
                    )
                    firmware = Firmware(args.bridge, lab=mode != "blocked")
                    try:
                        async with client() as device:
                            await device.subscribe(base + "/control/command", qos=1)
                            await device.subscribe(base + "/control/reply", qos=1)
                            await gateway.send_control_command(
                                "test_house", "test_relay", identifier
                            )
                            candidate = await receive(device, base + "/control/command")
                            if mode == "delayed-command":
                                await asyncio.sleep(0.8)
                            value = firmware.op("candidate", candidate)
                            req = value["request"]
                            if mode == "expired":
                                # Candidate delivery/application held until original E.
                                timer.offset += 30_000_000
                            if mode == "bad-digest":
                                req["intent_sha256"] = "0" * 64
                            if mode == "restart":
                                await gateway.stop()
                                with Session(engine) as s:
                                    authority = ControlAuthority(
                                        timer.clock(), gate=lambda *unused: None
                                    )
                                    authority.initialize(s)
                                gateway.control_authority = authority
                                gateway.start()
                                await ready()
                            encoded = json.dumps(req, separators=(",", ":"))
                            await device.publish(
                                base + "/control/request", encoded, qos=1
                            )
                            reply = await receive(device, base + "/control/reply")
                            denied = mode in (
                                "expired",
                                "delayed-command",
                                "restart",
                                "bad-digest",
                            )
                            assert json.loads(reply)["type"] == (
                                "permit_denied" if denied else "permit_grant"
                            ), (mode, reply)
                            if mode == "duplicate-request":
                                await asyncio.sleep(1.05)
                                await device.publish(
                                    base + "/control/request", encoded, qos=1
                                )
                                assert (
                                    await receive(device, base + "/control/reply")
                                    == reply
                                )
                            if mode == "delayed-reply":
                                await asyncio.sleep(2.1)
                            if mode in ("reconnect", "reboot", "clock-fault"):
                                firmware.op(mode)
                        # A fresh authenticated MQTT session receives a replayed old grant.
                        if mode in ("reconnect", "reboot"):
                            async with (
                                client() as reconnected,
                                client("core_lab") as server,
                            ):
                                await reconnected.subscribe(
                                    base + "/control/reply", qos=1
                                )
                                await server.publish(
                                    base + "/control/reply", reply, qos=1
                                )
                                reply = await receive(
                                    reconnected, base + "/control/reply"
                                )
                        result = firmware.op("reply", reply)
                        expected = mode in (
                            "apply",
                            "duplicate-request",
                            "replay",
                            "late-ack",
                        )
                        assert result["writes"] == int(expected), (mode, result)
                        assert firmware.op("legacy", candidate)["writes"] == int(
                            expected
                        )
                        if mode == "replay":
                            assert firmware.op("reply", reply)["writes"] == 1
                            assert firmware.op("candidate", candidate)["writes"] == 1
                            with Session(engine) as s:
                                altered = decode_request(encoded.encode()).model_copy(
                                    update={"nonce": "f" * 32}
                                )
                                try:
                                    authority.request(
                                        s, "test_house", "test_relay", altered
                                    )
                                except ConflictError:
                                    pass
                                else:
                                    raise AssertionError("Replay issued another permit")
                        if mode == "late-ack":
                            timer.offset += 30_000_000
                        if result["status"] in ("applied", "rejected", "failed"):
                            c = json.loads(candidate)
                            async with client() as reporter:
                                await reporter.publish(
                                    base + "/ack",
                                    json.dumps(
                                        {
                                            "command_id": identifier,
                                            "correlation_id": c["correlation_id"],
                                            "status": result["status"],
                                        }
                                    ),
                                    qos=1,
                                )
                                await reporter.publish(
                                    base + "/state",
                                    json.dumps(
                                        {
                                            "timestamp": timer.wall().isoformat(),
                                            "correlation_id": c["correlation_id"],
                                            "state": {"on": result["on"]},
                                        }
                                    ),
                                    qos=1,
                                )
                            async with asyncio.timeout(5):
                                while ("state", c["correlation_id"]) not in evidence:
                                    await asyncio.sleep(0.01)
                        if mode == "restart":
                            with Session(engine) as s:
                                try:
                                    CommandService(
                                        CommandRepository(s),
                                        control_authority=authority,
                                    ).get("test_house", identifier)
                                except UnavailableError:
                                    pass
                                else:
                                    raise AssertionError("Restart fence failed")
                            timer.offset += authority.clock.fence_us
                        with Session(engine) as s:
                            receipt = CommandService(
                                CommandRepository(s), control_authority=authority
                            ).get("test_house", identifier)
                            if mode in (
                                "late-ack",
                                "expired",
                                "delayed-command",
                                "restart",
                            ):
                                assert receipt.status == "timed_out"
                            if mode == "late-ack":
                                assert (
                                    receipt.ack_status is None
                                    and receipt.state_received_at is None
                                )
                            if mode == "apply":
                                assert receipt.status == "applied"
                        print(
                            "MQTT E2E: " + mode + " PASS (recording GPIO only)",
                            flush=True,
                        )
                    finally:
                        firmware.close()
                # Retained bootstrap delivery is visible to the real Core adapter.
                await asyncio.sleep(1.05)
                identifier = prepare()
                firmware = Firmware(args.bridge)
                try:
                    async with client() as device:
                        await device.subscribe(base + "/control/command", qos=1)
                        await device.subscribe(base + "/control/reply", qos=1)
                        await gateway.send_control_command(
                            "test_house", "test_relay", identifier
                        )
                        candidate = await receive(device, base + "/control/command")
                        req = firmware.op("candidate", candidate)["request"]
                        await gateway.stop()
                        await device.publish(
                            base + "/control/request",
                            json.dumps(req),
                            qos=1,
                            retain=True,
                        )
                        gateway.start()
                        await ready()
                        await empty(device, base + "/control/reply")
                        with Session(engine) as s:
                            assert (
                                s.get(
                                    ControlCommandORM, identifier
                                ).permit_binding_sha256
                                is None
                            )
                        await device.publish(
                            base + "/control/request", b"", qos=1, retain=True
                        )
                        assert firmware.op("clock-fault")["writes"] == 0
                    print(
                        "MQTT REAL: retained request bootstrap replay ignored, no binding/GPIO PASS",
                        flush=True,
                    )
                finally:
                    firmware.close()
                # Real legacy software simulator (unchanged Core code) reports
                # applied ACK/state over TLS; no fake physical simulator identity.
                async with client("software_lab") as software:
                    from simulator.mqtt_relay import Relay

                    topic = "kzhome/v1/test_house/software_relay/set"
                    await software.subscribe(topic, qos=1)
                    now = datetime.now(UTC)
                    try:
                        await gateway.send_command(
                            "test_house",
                            "test_relay",
                            {"on": True},
                            timestamp=now,
                            expires_at=now + timedelta(seconds=30),
                        )
                    except ConflictError:
                        pass
                    else:
                        raise AssertionError("Legacy physical gateway guard bypassed")
                    now = datetime.now(UTC)
                    await gateway.send_command(
                        "test_house",
                        "software_relay",
                        {"on": True},
                        timestamp=now,
                        expires_at=now + timedelta(seconds=30),
                    )
                    payload = await receive(software, topic)
                    command_payload = json.loads(payload)
                    assert (
                        command_payload["state"] == {"on": True}
                        and "expires_at" in command_payload
                    )
                    relay = Relay("test_house", "software_relay", software.publish)
                    await relay.handle(topic, payload.encode())
                    async with asyncio.timeout(5):
                        while True:
                            with Session(engine) as s:
                                if s.get(DeviceORM, "software_relay").state == {
                                    "on": True
                                }:
                                    break
                            await asyncio.sleep(0.01)
                    assert relay.on
                print(
                    "MQTT REAL: legacy software set routing PASS; physical legacy no GPIO PASS",
                    flush=True,
                )
            finally:
                await gateway.stop()

        if sys.platform == "win32":
            asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
        asyncio.run(test_network())

        identifier = prepare()
        with Session(engine) as s:
            authority.stage(s, "test_house", "test_relay", identifier)
        env = os.environ.copy()
        env["KZ_LAB_DATABASE_URL"] = services.url
        env["KZ_LAB_RUN"] = services.name
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(Path(__file__).with_name("control_restart_probe.py")),
                "--core-root",
                str(args.core_root),
                "--command",
                identifier,
            ],
            env=env,
            check=True,
        )

        # Two concurrent database transactions contest one authority binding.
        timer.offset += 2_000_000
        identifier = prepare()
        with Session(engine) as s:
            delivery = authority.stage(s, "test_house", "test_relay", identifier)
        c = json.loads(delivery.payload)

        def compete(nonce):
            request = decode_request(
                json.dumps(
                    dict(
                        control_version=1,
                        type="permit_request",
                        execution_profile="deadline-permit-v1",
                        core_epoch=c["core_epoch"],
                        command_id=identifier,
                        boot_id="1" * 32,
                        session_id="2" * 32,
                        nonce=nonce,
                        intent_sha256=authority_module.digest(delivery.payload),
                    )
                ).encode()
            )
            with Session(engine) as s:
                try:
                    return authority.request(
                        s, "test_house", "test_relay", request
                    ).grant
                except (ConflictError, UnavailableError):
                    return False

        with ThreadPoolExecutor(max_workers=2) as pool:
            assert sum(pool.map(compete, ("3" * 32, "4" * 32))) == 1
        # Independent SQL CAS races validate rowcount + durable unique constraint.
        identifier = prepare()

        def cas(binding):
            with engine.begin() as c:
                return c.execute(
                    text(
                        "UPDATE command_control SET permit_binding_sha256=:b "
                        "WHERE command_id=:i AND permit_binding_sha256 IS NULL"
                    ),
                    {"b": binding, "i": identifier},
                ).rowcount

        with ThreadPoolExecutor(max_workers=2) as pool:
            assert sum(pool.map(cas, ("5" * 64, "6" * 64))) == 1
        with Session(engine) as s:
            used = s.get(ControlCommandORM, identifier).permit_binding_sha256
        other = prepare()
        with Session(engine) as s:
            s.get(ControlCommandORM, other).permit_binding_sha256 = used
            try:
                s.commit()
            except IntegrityError:
                s.rollback()
            else:
                raise AssertionError("Nonce-context UNIQUE constraint accepted reuse")
        print(
            "POSTGRES REAL: concurrent authority issuance and independent CAS races PASS",
            flush=True,
        )

        # Clock faults are deterministic injections, never host suspend/time changes.
        for mode in (
            "wall-jump",
            "suspend-stopped-timer",
            "rollback",
            "unqualified",
            "health-loss",
        ):
            t = Time()
            a = ControlAuthority(
                t.clock(qualified=mode != "unqualified"), gate=lambda *unused: None
            )
            with Session(engine) as s:
                a.initialize(s)
            t.offset += a.clock.fence_us
            if mode in ("wall-jump", "suspend-stopped-timer"):
                t.wall_offset += 60_000_000
            if mode == "rollback":
                t.offset = -1_000_000
            if mode == "health-loss":
                t.health = False
            try:
                a.clock.sample()
            except UnavailableError:
                pass
            else:
                raise AssertionError("Unhealthy timing admitted " + mode)
        closed = ControlAuthority(timer.clock())
        with Session(engine) as s:
            try:
                closed.physical(s, "test_house", "test_relay")
            except ConflictError:
                pass
            else:
                raise AssertionError("Production physical gate opened")
        print(
            "FAULT SIMULATION: wall jump, suspend gap, rollback, unqualified bounds, health loss; default gate closed PASS",
            flush=True,
        )
        env = os.environ.copy()
        env["TEST_POSTGRESQL_URL"] = services.url
        subprocess.run(
            [
                sys.executable,
                "-B",
                "-m",
                "pytest",
                "-q",
                *([] if args.full_core else ["tests/test_schema.py"]),
            ],
            cwd=args.core_root,
            env=env,
            check=True,
        )
        print(
            "PASS: disposable real-service integration completed; no physical enablement",
            flush=True,
        )
    finally:
        if engine is not None:
            engine.dispose()
        services.close()


if __name__ == "__main__":
    main()
