"""Load actual read-only Core schema/gateway; never open DB, broker or Docker."""
import argparse
import asyncio
from datetime import UTC, datetime, timedelta
import json
from pathlib import Path
import sys
from types import SimpleNamespace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-root", required=True, type=Path)
    parser.add_argument("--site-packages", type=Path)
    parser.add_argument("--messages", required=True, type=Path)
    args = parser.parse_args()
    # Core is read-only, including __pycache__. Dependencies must already exist.
    sys.dont_write_bytecode = True
    if args.site_packages:
        sys.path.insert(0, str(args.site_packages.resolve()))
    sys.path.insert(0, str(args.core_root.resolve()))

    from app.events import EventBus
    from app.services.command_service import COMMAND_TIMEOUT_SECONDS, CommandService
    from app.transports.mqtt import MQTTGateway
    from app.transports.mqtt_client import FakeMQTTClient, MQTTMessage
    from app.transports.mqtt_models import AckEnvelope, StateEnvelope, StatusEnvelope

    messages = [json.loads(line) for line in args.messages.read_text(encoding="utf-8").splitlines()]
    assert messages and messages[0]["status"] == "offline"
    assert messages[0]["last_seen"] == "2026-10-30T12:00:00.000Z"
    assert any(m.get("last_seen") == "2026-10-30T12:01:00.000Z" for m in messages)
    assert {m["status"] for m in messages} == {"online", "offline"}
    for payload in messages:
        envelope = StatusEnvelope.model_validate(payload)
        assert envelope.protocol_version == "v1" and envelope.last_seen.tzinfo is not None
        if payload["status"] == "offline":
            assert "timestamp" not in payload  # No fictitious disconnect event time.

    async def gateway_tests():
        statuses, identities, events = [], [], []

        async def identity(house, device):
            assert (house, device) == ("test_house", "test_relay")
            identities.append((house, device))

        async def state(*_):
            raise AssertionError("Status must not route into state")

        async def status(house, device, online, last_seen):
            statuses.append((online, last_seen))
            return True

        async def event(event):
            events.append(event)

        bus = EventBus()
        bus.subscribe(event)
        client = FakeMQTTClient()
        await client.connect()
        gateway = MQTTGateway(client, bus, state, status, identity)
        await gateway.send_command("test_house", "test_relay", {"on": True},
                                   command_id="deadline_probe", correlation_id="deadline_probe")
        command_topic, command_json, qos, retain = client.published[-1]
        assert command_topic.endswith("/set") and qos == 1 and not retain
        assert set(json.loads(command_json)) == {
            "protocol_version", "command_id", "correlation_id", "timestamp", "state"}
        topic = "kzhome/v1/test_house/test_relay/status"
        for payload in messages:
            # Exercise both live status and retained bootstrap/replay paths.
            for retained in (False, True):
                await gateway.handle_message(MQTTMessage(topic, json.dumps(payload).encode(), retained))
                assert statuses[-1][0] == (payload["status"] == "online")
                assert statuses[-1][1].isoformat().startswith(payload["last_seen"][:19])
        assert len(statuses) == len(messages) * 2 == len(identities) == len(events)
        before = len(statuses)
        bad = [b"{", b"[]", b'{"status":"offline"}',
            b'{"status":"offline","last_seen":"invalid"}',
            b'{"status":"unknown","last_seen":"2026-10-30T12:00:00Z"}',
            b'{"status":"online","last_seen":"2026-10-30T12:00:00Z","heartbeat_interval_seconds":1}',
            b'{"status":"offline","last_seen":"2026-10-30T12:00:00Z","protocol_version":"v2"}']
        for payload in bad:
            try:
                await gateway.handle_message(MQTTMessage(topic, payload, False))
            except ValueError:
                pass
            else:
                raise AssertionError("Malformed status accepted")
        assert len(statuses) == before

    asyncio.run(gateway_tests())

    # Actual Core expiry/evidence logic: no database or network needed. Paired
    # with the C engine's 60s first-delivery test, this exposes the unresolved gap.
    start = datetime(2026, 10, 30, 12, tzinfo=UTC)
    assert COMMAND_TIMEOUT_SECONDS == 30
    row = SimpleNamespace(status="dispatched", expires_at=start + timedelta(seconds=COMMAND_TIMEOUT_SECONDS),
        dispatch_started_at=start, ack_status=None, state_received_at=None,
        reported_on=None, requested_on=True)

    class Repository:
        def find(self, **_):
            return row

        def save(self):
            pass

    clock = [start + timedelta(seconds=31)]
    service = CommandService(Repository(), now=lambda: clock[0])
    service.expire(row)
    assert row.status == "timed_out"
    clock[0] = start + timedelta(seconds=60)
    service.ack("test_house", "test_relay", AckEnvelope(command_id="late",
        correlation_id="late", status="applied", timestamp=start + timedelta(seconds=60)))
    service.state("test_house", "test_relay", StateEnvelope(correlation_id="late",
        timestamp=start + timedelta(seconds=60), state={"on": True}))
    assert row.status == "timed_out" and row.ack_status is None and row.state_received_at is None
    print(f"PASS: {len(messages)} firmware statuses accepted by actual Core schema/gateway; "
          "malformed statuses rejected; late command evidence leaves timed_out terminal")


if __name__ == "__main__":
    main()
