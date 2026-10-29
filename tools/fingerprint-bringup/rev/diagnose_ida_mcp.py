"""Print raw IDA MCP startup replies hidden by REA's output validation.

Run with the Python interpreter used by idalib-mcp (it provides `mcp`):
  ~/.local/share/uv/tools/ida-pro-mcp/bin/python diagnose_ida_mcp.py

Uses a private input copy; never connects to the physical device.
"""

import argparse
import asyncio
import json
import os
from pathlib import Path
import shutil
import tempfile
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def payload(reply):
    if reply.structuredContent is not None:
        return reply.structuredContent
    for block in reply.content:
        if block.type == "text":
            try:
                return json.loads(block.text)
            except ValueError:
                pass
    return reply.model_dump(mode="json", exclude_none=True)


async def diagnose(args):
    config = json.loads(args.config.read_text())
    directory = Path(tempfile.mkdtemp(prefix="ida-diagnose-")).resolve()
    target = directory / args.target.name
    released = True
    try:
        shutil.copy2(args.target, target)
        params = StdioServerParameters(
            command=config["command"],
            args=config.get("args", []),
            env={**os.environ, **config.get("env", {})},
        )
        async with stdio_client(params) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                await session.initialize()
                await session.list_tools()
                session_id = "rea-" + str(uuid.uuid4())
                released = False
                try:
                    result = payload(await session.call_tool("idb_open", {
                        "input_path": str(target),
                        "mode": "force_headless",
                        "run_auto_analysis": True,
                        "build_caches": True,
                        "init_hexrays": True,
                        "preferred_session_id": session_id,
                    }))
                    print(json.dumps({"idb_open": result}, ensure_ascii=False, indent=2), flush=True)
                finally:
                    inventory = payload(await session.call_tool("idb_list", {}))
                    for entry in inventory.get("sessions", []):
                        if entry.get("session_id") == session_id or entry.get("input_path") == str(target):
                            closed = payload(await session.call_tool("idb_close", {
                                "database": entry["session_id"], "save": False,
                            }))
                            print(json.dumps({"idb_close": closed}, ensure_ascii=False), flush=True)
                    inventory = payload(await session.call_tool("idb_list", {}))
                    released = "sessions" in inventory and not any(
                        entry.get("session_id") == session_id or entry.get("input_path") == str(target)
                        for entry in inventory["sessions"]
                    )
    finally:
        if released:
            shutil.rmtree(directory)
        else:
            print(f"Cleanup unverified; retained private input: {directory}", flush=True)


if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path, nargs="?", default=root / "cabs/qctree/fingerpr.mbn")
    parser.add_argument("--config", type=Path, default=Path(os.environ.get("REA_IDA_MCP_CONFIG", root / "ida-mcp.json")))
    asyncio.run(diagnose(parser.parse_args()))
