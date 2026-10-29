"""Local REA 6.8.0 workaround: preserve an idb_open rejection's actual error.

Usage: python3 patch_rea_ida_open_error.py [--restore]
Reinstalling/upgrading REA replaces this workaround. Originals are kept beside
each modified file with suffix .before-ida-open-error-fix.
"""

import argparse
from pathlib import Path
import shutil


ROOT = Path("/opt/homebrew/lib/node_modules/rea-agents/dist")
MARKER = "// Local fix: preserve IDA idb_open rejection diagnostics."
SUFFIX = ".before-ida-open-error-fix"
INSERT = '''// Local fix: preserve IDA idb_open rejection diagnostics.
if (this.#openRejected) {
    throw new ProviderAdapterError("ida", "idb_open", {
        userMessage: `IDA MCP could not open the analysis database: ${raw.error}`,
        diagnostics: { reason: raw.error, startup_operation: "idb_open" },
    });
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args()
    files = [ROOT / "ida/IdaSessionClient.js"]
    files += sorted((ROOT / "cli-runtime").glob("binary-*.js"))
    files += sorted((ROOT / "mcp").glob("binary-*.js"))
    edits = []
    for path in files:
        backup = path.with_name(path.name + SUFFIX)
        source = path.read_text()
        if args.restore:
            if backup.exists() and MARKER in source:
                edits.append((path, backup.read_text()))
            continue
        if MARKER in source:
            print(f"Already patched: {path}")
            continue
        if "this.#openRejected = rejectedOpenDatabaseSchema.safeParse(raw).success;" not in source:
            continue
        needle = "const opened = openDatabaseSchema.parse(raw);"
        if source.count(needle) != 1:
            raise SystemExit(f"Unexpected REA layout, no files changed: {path}")
        if backup.exists():
            if backup.read_text() != source:
                raise SystemExit(f"Backup differs; refusing to overwrite: {backup}")
        edits.append((path, source.replace(needle, INSERT + needle)))
    for path, content in edits:
        backup = path.with_name(path.name + SUFFIX)
        if not args.restore and not backup.exists():
            shutil.copy2(path, backup)
        path.write_text(content)
        print(f"{'Restored' if args.restore else 'Patched'}: {path}")


if __name__ == "__main__":
    main()
