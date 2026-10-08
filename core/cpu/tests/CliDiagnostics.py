"""Parse CLI diagnostics without hiding unexpected native stderr output."""

import json
import os
import re
import sys


_VALIDATION_ANNOUNCEMENT = re.compile(
    r"[0-9]{4}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01]) "
    r"(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]\.[0-9]{3} "
    r"anyps5_cpu_run\[[1-9][0-9]*:[1-9][0-9]*\] "
    r"Metal (API|GPU) Validation Enabled"
)
_VALIDATION_ENVIRONMENT = {"API": "MTL_DEBUG_LAYER", "GPU": "MTL_SHADER_VALIDATION"}


def _invalid_constant(value):
    raise ValueError(f"Invalid JSON constant: {value}")


def parse_diagnostics(stderr, environment=None, platform=None):
    environment = os.environ if environment is None else environment
    platform = sys.platform if platform is None else platform
    events = []
    for number, line in enumerate(stderr.splitlines(), 1):
        announcement = _VALIDATION_ANNOUNCEMENT.fullmatch(line)
        if (platform == "darwin" and announcement is not None
                and environment.get(_VALIDATION_ENVIRONMENT[announcement.group(1)]) == "1"):
            continue
        try:
            event = json.loads(line, parse_constant=_invalid_constant)
        except ValueError as error:
            raise AssertionError(
                f"Invalid CLI diagnostic on stderr line {number}: {error}\nFull stderr:\n{stderr}"
            ) from error
        if (not isinstance(event, dict)
                or type(event.get("schema_version")) is not int
                or event["schema_version"] != 1):
            raise AssertionError(
                f"Expected a schema-1 diagnostic object on stderr line {number}\nFull stderr:\n{stderr}"
            )
        events.append(event)
    if not events:
        raise AssertionError(f"Missing CLI diagnostic records\nFull stderr:\n{stderr}")
    return events
