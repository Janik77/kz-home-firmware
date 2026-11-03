"""Fresh Core process recovery probe, used only by service_integration.py."""

import argparse
from datetime import UTC, datetime, timedelta
import os
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-root", required=True)
    parser.add_argument("--command", required=True)
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, args.core_root)
    from sqlalchemy import create_engine
    from sqlalchemy.engine import make_url
    from sqlalchemy.orm import Session
    from app.models import ControlCommandORM, DeviceCommandORM
    from app.repositories.command_repository import CommandRepository
    from app.services.command_service import CommandService
    from app.services.control_authority import ControlAuthority
    from app.services.control_clock import ControlClock
    from app.core.errors import ConflictError, UnavailableError
    from app.transports.control_models import decode_request
    import json

    url = make_url(os.environ["KZ_LAB_DATABASE_URL"])
    assert url.host == "127.0.0.1" and url.database == "lab" and url.username == "lab"
    assert os.environ["KZ_LAB_RUN"].startswith("kz-control-lab-")
    tick = [0]
    # Deliberately unrelated boot UTC: recovery must depend on fence, not wall E.
    anchor = datetime(2030, 1, 1, tzinfo=UTC)
    clock = ControlClock(
        ticks=lambda: tick[0],
        wall=lambda: anchor + timedelta(microseconds=tick[0]),
        qualified=True,
        utc_health=lambda: True,
    )
    authority = ControlAuthority(clock, gate=lambda *unused: None)
    engine = create_engine(url)
    try:
        with Session(engine) as s:
            authority.initialize(s)
            record = s.get(ControlCommandORM, args.command)
            assert (
                record.authority_epoch != clock.epoch
                and not authority.windows
                and not authority.replies
            )
            request = decode_request(
                json.dumps(
                    dict(
                        control_version=1,
                        type="permit_request",
                        execution_profile="deadline-permit-v1",
                        core_epoch=record.authority_epoch,
                        command_id=args.command,
                        boot_id="a" * 32,
                        session_id="b" * 32,
                        nonce="c" * 32,
                        intent_sha256=record.wire_sha256,
                    )
                ).encode()
            )
            service = CommandService(CommandRepository(s), control_authority=authority)
            try:
                service.get("test_house", args.command)
            except UnavailableError:
                pass
            else:
                raise AssertionError("Fresh process expired before fence")
            assert s.get(DeviceCommandORM, args.command).status in (
                "queued",
                "dispatched",
            )
            tick[0] = clock.fence_us
            try:
                authority.request(s, "test_house", "test_relay", request)
            except ConflictError:
                pass
            else:
                raise AssertionError("Fresh process reissued old grant")
            assert service.get("test_house", args.command).status == "timed_out"
        print(
            "POSTGRES REAL: fresh Core subprocess, lost RAM, new epoch, no reissue; simulated recovery fence PASS"
        )
    finally:
        engine.dispose()


if __name__ == "__main__":
    main()
