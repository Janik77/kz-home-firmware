"""Real loopback HTTP Core application + existing software Relay on fixture services.

No ASGITransport, dependency overrides, fake MQTT, gate patches or physical relabeling.
Only the caller's disposable database/broker and generated human credentials are used.
"""

import asyncio
from builtins import anext
from contextlib import suppress
import os
import secrets
import socket
import ssl
from unittest.mock import patch
from uuid import uuid4


async def check_http(services, engine):
    import aiomqtt
    import httpx
    import uvicorn
    from sqlalchemy import func, select
    from sqlalchemy.orm import Session
    from app.core import Settings
    from app.models import DeviceCommandORM, HouseMembershipORM
    from app.repositories import UserRepository
    from app.services.auth_service import UserService
    from app.transports.mqtt_client import AiomqttClient
    from simulator.mqtt_relay import Relay

    password = secrets.token_urlsafe(32)
    jwt_secret = secrets.token_urlsafe(48)
    with Session(engine) as session:
        user = UserService(UserRepository(session)).create(
            "http-owner@example.test", password
        )
        session.add(
            HouseMembershipORM(
                id=str(uuid4()), user_id=user.id, house_id="test_house", role="owner"
            )
        )
        session.commit()

    # app.main's module-level application must also avoid .env loading on import.
    # All values are ephemeral fixture inputs, never inherited installation settings.
    env = {
        "APP_ENV": "production",
        "DATABASE_URL": services.url,
        "MQTT_ENABLED": "true",
        "MQTT_TLS_ENABLED": "true",
        "MQTT_HOST": "localhost",
        "MQTT_PORT": str(services.mqtt_port),
        "MQTT_CLIENT_ID": "unused-import-app",
        "AUTH_JWT_SECRET": jwt_secret,
    }
    with patch.dict(os.environ, env, clear=True):
        from app.main import create_app

    settings = Settings(
        database_url=services.url,
        app_env="production",
        auth_jwt_secret=jwt_secret,
        mqtt_enabled=True,
        mqtt_host="localhost",
        mqtt_port=services.mqtt_port,
        mqtt_tls_enabled=True,
        mqtt_client_id="http-core-" + uuid4().hex,
        mqtt_username="core_lab",
        mqtt_password=services.password,
        mqtt_software_devices=(("test_house", "software_relay"),),
    )
    ctx = ssl.create_default_context(cafile=str(services.directory / "ca.crt"))
    wire = AiomqttClient(
        host="localhost",
        port=services.mqtt_port,
        username="core_lab",
        password=services.password,
        tls_enabled=True,
        keepalive=15,
        client_id=settings.mqtt_client_id,
    )
    wire._options["tls_context"] = ctx
    app = create_app(settings, run_simulator=False, mqtt_client=wire)
    server = uvicorn.Server(
        uvicorn.Config(
            app, log_level="warning", access_log=False, lifespan="on", loop="asyncio"
        )
    )
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    serve_task = asyncio.create_task(server.serve(sockets=[sock]))
    relay_task = None
    try:
        async with asyncio.timeout(15):
            while not server.started or not app.state.mqtt_gateway.connected:
                if serve_task.done():
                    await serve_task
                    raise AssertionError("HTTP server exited before readiness")
                await asyncio.sleep(0.02)
        async with (
            aiomqtt.Client(
                "localhost",
                port=services.mqtt_port,
                username="software_lab",
                password=services.password,
                tls_context=ctx,
                identifier="http-relay-" + uuid4().hex,
            ) as device,
            aiomqtt.Client(
                "localhost",
                port=services.mqtt_port,
                username="physical_lab",
                password=services.password,
                tls_context=ctx,
                identifier="http-observer-" + uuid4().hex,
            ) as observer,
            httpx.AsyncClient(base_url=f"http://127.0.0.1:{port}", timeout=5) as http,
        ):
            await device.subscribe("kzhome/v1/test_house/software_relay/set", qos=1)
            await observer.subscribe(
                "kzhome/v1/test_house/test_relay/control/command", qos=1
            )
            relay = Relay("test_house", "software_relay", device.publish)
            commands = []

            async def serve_relay():
                async for message in device.messages:
                    commands.append(bytes(message.payload))
                    await relay.handle(str(message.topic), bytes(message.payload))

            relay_task = asyncio.create_task(serve_relay())
            assert (await http.get("/ready")).status_code == 200
            path = "/v1/devices/software_relay/commands"
            assert (
                await http.post(
                    path, json={"on": True}, headers={"Idempotency-Key": str(uuid4())}
                )
            ).status_code == 401
            login = await http.post(
                "/auth/login",
                json={"email": "http-owner@example.test", "password": password},
            )
            assert login.status_code == 200
            auth = {"Authorization": "Bearer " + login.json()["access_token"]}
            for count, on in enumerate((True, False), start=1):
                headers = {**auth, "Idempotency-Key": str(uuid4())}
                response = await http.post(path, json={"on": on}, headers=headers)
                assert response.status_code == 202, response.status_code
                identifier = response.json()["command_id"]
                async with asyncio.timeout(10):
                    while True:
                        response = await http.get(
                            f"/v1/houses/test_house/commands/{identifier}", headers=auth
                        )
                        assert response.status_code == 200
                        receipt = response.json()
                        if receipt["status"] == "applied":
                            break
                        assert receipt["status"] in ("queued", "dispatched")
                        await asyncio.sleep(0.02)
                assert receipt["ack_status"] == "applied"
                assert receipt["requested_on"] is on and receipt["reported_on"] is on
                assert receipt["ack_received_at"] and receipt["state_received_at"]
                assert relay.on is on and len(commands) == count
                retry = await http.post(path, json={"on": on}, headers=headers)
                assert (
                    retry.status_code == 202
                    and retry.json()["command_id"] == identifier
                )
                await asyncio.sleep(0.1)
                assert len(commands) == count
                print(
                    f"HTTP REAL: {'ON' if on else 'OFF'} -> existing software Relay -> "
                    "MQTT ACK/state -> HTTP applied; idempotent retry PASS",
                    flush=True,
                )

            def physical_count():
                with Session(engine) as session:
                    return session.scalar(
                        select(func.count())
                        .select_from(DeviceCommandORM)
                        .where(DeviceCommandORM.device_id == "test_relay")
                    )

            before = physical_count()
            for on in (True, False):
                response = await http.post(
                    "/v1/devices/test_relay/commands",
                    json={"on": on},
                    headers={**auth, "Idempotency-Key": str(uuid4())},
                )
                assert response.status_code == 409
                assert (
                    response.json()["detail"]
                    == "Physical command expiry contract is not verified"
                )
            assert physical_count() == before
            try:
                async with asyncio.timeout(0.3):
                    await anext(observer.messages)
            except TimeoutError:
                pass
            else:
                raise AssertionError("Physical HTTP request emitted a control command")
            print(
                "HTTP REAL: authenticated physical ON/OFF denied 409 before ledger/publication; "
                "production gates unchanged PASS",
                flush=True,
            )
    finally:
        if relay_task is not None:
            relay_task.cancel()
            with suppress(asyncio.CancelledError):
                await relay_task
        server.should_exit = True
        try:
            await asyncio.wait_for(serve_task, timeout=10)
        finally:
            sock.close()
