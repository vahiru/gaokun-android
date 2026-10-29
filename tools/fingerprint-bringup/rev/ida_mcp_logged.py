"""Run the installed IDA MCP supervisor while retaining worker diagnostics.

Use the idalib-mcp Python interpreter. Logs are owner-only files under
the system temporary directory; the underlying MCP protocol is unchanged.
"""

import http.client
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from ida_pro_mcp import idalib_supervisor as supervisor


_popen = subprocess.Popen
_rpc = supervisor.IdalibSupervisor._worker_rpc
_terminate = supervisor.IdalibSupervisor._terminate_worker


def logged_popen(command, *args, **kwargs):
    if isinstance(command, list) and "ida_pro_mcp.idalib_server" in command:
        directory = Path(tempfile.gettempdir()) / f"rea-ida-worker-logs-{os.getuid()}"
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        fd, path = tempfile.mkstemp(prefix="worker-", suffix=".log", dir=directory)
        user_directory = Path(tempfile.mkdtemp(prefix="user-", dir=directory))
        try:
            with os.fdopen(fd, "ab", buffering=0) as log:
                environment = dict(kwargs.get("env", os.environ))
                source = Path(environment.get("IDAUSR") or Path.home() / ".idapro")
                # IDA writes ida.reg even when only reading an analysis target.
                # Keep those writes inside the sandbox's writable temporary tree.
                # The private license copy is deleted once the owned worker exits.
                settings = [source / name for name in ("ida.reg", "ida-config.json", "proccache.lst")]
                settings += list(source.glob("*.hexlic"))
                for setting in settings:
                    if setting.is_file():
                        destination = user_directory / setting.name
                        shutil.copyfile(setting, destination)
                        destination.chmod(0o600)
                kwargs["stdout"] = log
                kwargs["stderr"] = log
                kwargs["env"] = {
                    **environment,
                    "IDAUSR": str(user_directory),
                    "PYTHONFAULTHANDLER": "1",
                    "PYTHONUNBUFFERED": "1",
                }
                # The worker's verbose mode also enables IDA console messages.
                command = [*command, "--verbose"]
                process = _popen(command, *args, **kwargs)
            process.ida_diagnostic_log = path
            process.ida_user_directory = user_directory
            print(f"IDA worker pid={process.pid} log={path}", file=sys.stderr, flush=True)
            return process
        except BaseException:
            shutil.rmtree(user_directory)
            print(f"IDA worker launch failed; log={path}", file=sys.stderr, flush=True)
            raise
    return _popen(command, *args, **kwargs)


def terminate_worker(self, worker):
    try:
        return _terminate(self, worker)
    finally:
        process = worker.process
        directory = getattr(process, "ida_user_directory", None)
        if directory is not None and process.poll() is not None:
            shutil.rmtree(directory, ignore_errors=True)


def logged_rpc(self, worker, payload, **kwargs):
    try:
        return _rpc(self, worker, payload, **kwargs)
    except (http.client.HTTPException, ConnectionError) as exc:
        process = worker.process
        path = getattr(process, "ida_diagnostic_log", None)
        # Read the status before supervisor cleanup terminates the worker.
        status = process.poll() if process is not None else "unknown"
        detail = f"{exc}; worker_pid={getattr(process, 'pid', None)}; exit_status={status}; log={path}"
        if path:
            try:
                with open(path, "rb") as log:
                    log.seek(0, os.SEEK_END)
                    log.seek(max(0, log.tell() - 6000))
                    detail += "\nWorker log tail:\n" + log.read().decode("utf-8", errors="replace")
            except OSError as log_error:
                detail += f"; could not read log: {log_error}"
        raise RuntimeError(detail) from exc


if __name__ == "__main__":
    subprocess.Popen = logged_popen
    supervisor.IdalibSupervisor._worker_rpc = logged_rpc
    supervisor.IdalibSupervisor._terminate_worker = terminate_worker
    supervisor.main()
