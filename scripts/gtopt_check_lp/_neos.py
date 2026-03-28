# SPDX-License-Identifier: BSD-3-Clause
"""NEOS optimization server client for gtopt_check_lp.

Submits infeasible LP files to the NEOS CPLEX LP solver via its XML-RPC API
and waits for the conflict-analysis results.

CPLEX Interactive Optimizer command reference
---------------------------------------------
After an infeasible LP solve the correct command sequence to identify the
minimal infeasible subsystem is:

1. ``set preprocessing presolve n``    – disable presolve so that infeasibility
   is detected by the simplex method rather than presolve; without this step
   the conflict refiner has no LP basis to work from.
2. ``optimize``                        – re-solve; simplex will confirm
   infeasibility and produce a dual infeasibility certificate.
3. ``tools conflict``                  – invoke the CPLEX conflict refiner
   (lives under the ``tools`` submenu in CPLEX 22.1+).
4. ``display conflict all``            – print the minimal conflict set
   (infeasible constraints and bound members).
"""

import http.client
import time
import urllib.request
import xmlrpc.client
from pathlib import Path
from typing import Optional

from ._compress import read_lp_text

_NEOS_DEFAULT_URL = "https://neos-server.org:3333"

# Size threshold (bytes) above which we warn that the LP may be too large
# for NEOS.  NEOS has an undocumented payload limit; large submissions
# typically time out or are rejected silently.
_NEOS_LP_SIZE_WARN = 50 * 1024 * 1024  # 50 MiB


def _check_internet(timeout: int = 5) -> bool:
    """Return True when basic internet connectivity is available.

    Tries a lightweight HTTPS HEAD request to a well-known host.  Returns
    False on any network error so callers can distinguish "no internet" from
    "NEOS-specific failure".
    """
    try:
        req = urllib.request.Request(
            "https://www.google.com",
            method="HEAD",
        )
        # S310: unverified HTTPS is acceptable for a connectivity check to
        # a well-known public host — no sensitive data is transmitted.
        with urllib.request.urlopen(req, timeout=timeout):  # noqa: S310
            return True
    except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        return False


def _diagnose_network_error(
    neos_url: str, lp_path: Path, error: Exception, timeout: int = 5
) -> str:
    """Return a human-readable diagnostic for a NEOS network failure.

    Inspects the error, checks basic internet connectivity, and checks the
    LP file size to give the user an actionable message rather than a raw
    traceback.
    """
    parts: list[str] = [f"Network error connecting to {neos_url}: {error}"]

    # 1. Check if the LP file is very large — NEOS may reject or time out.
    try:
        size = lp_path.stat().st_size
        if size > _NEOS_LP_SIZE_WARN:
            size_mb = size / (1024 * 1024)
            parts.append(
                f"  ⚠ The LP file is {size_mb:.1f} MiB — NEOS may reject or "
                "time out on large submissions."
            )
            parts.append("  Tip: try a local solver (--solver coinor) for large files.")
    except OSError:
        pass

    # 2. Check basic internet connectivity.
    if not _check_internet(timeout=timeout):
        parts.append(
            "  ✗ No internet connectivity detected (cannot reach www.google.com)."
        )
        parts.append("  Check your network connection and proxy settings.")
    else:
        # Internet works but NEOS is unreachable — service-specific issue.
        is_timeout = "timed out" in str(error).lower()
        if is_timeout:
            parts.append("  ✗ The NEOS server appears to be slow or unreachable.")
            parts.append(
                "  The connection timed out.  This may be a temporary issue; "
                "try again later or use --timeout with a larger value."
            )
        else:
            parts.append(
                "  ✗ Internet is reachable but the NEOS server returned an error."
            )
            parts.append(
                "  The NEOS service may be temporarily unavailable.  "
                "Try again later or use a local solver (--solver coinor)."
            )

    return "\n".join(parts)


# NEOS CPLEX LP XML template.
#
# The <post> section is executed *after* the initial LP read+solve.  Because
# NEOS typically runs with presolve on, the initial solve may terminate via
# presolve before the conflict refiner state is available.  We therefore:
#   1. Disable presolve.
#   2. Re-solve (simplex now confirms infeasibility).
#   3. Run the conflict refiner with `tools conflict`.
#   4. Display the conflict members.
_NEOS_LP_CPLEX_XML = """\
<document>
<category>lp</category>
<solver>CPLEX</solver>
<inputMethod>LP</inputMethod>
<email>{email}</email>
<LP><![CDATA[
{lp_content}
]]></LP>
<post><![CDATA[
set preprocessing presolve n
optimize
tools conflict
display conflict all
]]></post>
<comments><![CDATA[
LP infeasibility analysis submitted by gtopt_check_lp {version}
]]></comments>
</document>
"""


class _TimeoutTransport(xmlrpc.client.SafeTransport):
    """HTTPS XML-RPC transport with a configurable per-call socket timeout."""

    def __init__(self, timeout: int) -> None:
        super().__init__()
        self._timeout = timeout

    def make_connection(  # type: ignore[override]
        self, host: str
    ) -> http.client.HTTPSConnection:
        conn = super().make_connection(host)
        conn.timeout = self._timeout
        return conn


class NeosClient:
    """
    Client for the NEOS optimization server XML-RPC API.

    The NEOS XML-RPC endpoint is ``https://neos-server.org:3333``.
    See https://neos-server.org/neos/xml-rpc.html for the API reference.
    """

    def __init__(self, url: str = _NEOS_DEFAULT_URL, timeout: int = 10) -> None:
        self.url = url
        self.timeout = timeout
        self._proxy: Optional[xmlrpc.client.ServerProxy] = None

    def _get_proxy(self) -> xmlrpc.client.ServerProxy:
        if self._proxy is None:
            transport = _TimeoutTransport(timeout=self.timeout)
            self._proxy = xmlrpc.client.ServerProxy(self.url, transport=transport)
        return self._proxy

    def ping(self) -> bool:
        """Return True when the NEOS server is reachable.

        The NEOS ``ping()`` call returns ``"NeosServer is alive\\n"``.  We
        accept any response containing "alive" (case-insensitive) for
        robustness.
        """
        try:
            msg = self._get_proxy().ping()
            return "alive" in str(msg).lower()
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            return False

    def submit_lp(
        self, lp_path: Path, email: str, version: str = ""
    ) -> tuple[Optional[int], Optional[str]]:
        """
        Submit an LP file to NEOS for CPLEX conflict analysis.

        Accepts plain or gzip-compressed LP files.

        Returns ``(job_number, password)`` or ``(None, error_message)``.
        """
        lp_content = read_lp_text(lp_path)
        xml = _NEOS_LP_CPLEX_XML.format(
            email=email,
            lp_content=lp_content,
            version=version,
        )
        try:
            proxy = self._get_proxy()
            result = proxy.submitJob(xml)
            if isinstance(result, (list, tuple)) and len(result) == 2:
                job_number, password = result
                if isinstance(job_number, int) and job_number > 0:
                    return job_number, str(password)
                return None, f"NEOS returned error: {result!r}"
            return None, f"Unexpected NEOS response: {result!r}"
        except xmlrpc.client.Fault as exc:
            return None, f"NEOS XML-RPC fault: {exc}"
        except OSError as exc:
            return None, _diagnose_network_error(self.url, lp_path, exc)

    def wait_for_result(
        self,
        job_number: int,
        password: str,
        poll_interval: float = 5.0,
    ) -> tuple[bool, str]:
        """Poll until the NEOS job completes and return ``(success, output)``."""
        proxy = self._get_proxy()
        deadline = time.monotonic() + self.timeout
        last_status = ""

        while time.monotonic() < deadline:
            try:
                status = str(proxy.getJobStatus(job_number, password))
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                return False, f"Error polling NEOS: {exc}"

            if status != last_status:
                last_status = status

            if status == "Done":
                try:
                    raw = proxy.getFinalResults(job_number, password)
                    if isinstance(raw, xmlrpc.client.Binary):
                        output = raw.data.decode("utf-8", errors="replace")
                    elif isinstance(raw, bytes):
                        output = raw.decode("utf-8", errors="replace")
                    else:
                        output = str(raw)
                    return True, output
                except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                    return False, f"Error retrieving NEOS results: {exc}"

            if status in ("Error", "Unknown"):
                return False, f"NEOS job {job_number} ended with status: {status}"

            time.sleep(poll_interval)

        return (
            False,
            f"Timed out waiting for NEOS job {job_number} after {self.timeout}s.",
        )

    def submit_and_wait(
        self, lp_path: Path, email: str, poll_interval: float = 5.0, version: str = ""
    ) -> tuple[bool, str]:
        """Submit an LP to NEOS and wait for the conflict-analysis result."""
        if not self.ping():
            # Ping failed — diagnose whether it's a general internet issue
            # or a NEOS-specific problem.
            diag_parts: list[str] = [
                f"Cannot reach NEOS server at {self.url}.",
            ]
            if not _check_internet(timeout=5):
                diag_parts.append("  ✗ No internet connectivity detected.")
                diag_parts.append("  Check your network connection and proxy settings.")
            else:
                diag_parts.append(
                    "  Internet is reachable but the NEOS service is not responding."
                )
                diag_parts.append(
                    "  The NEOS server may be temporarily unavailable.  "
                    "Try again later or use a local solver (--solver coinor)."
                )
            # Also check file size for a helpful hint.
            try:
                size = lp_path.stat().st_size
                if size > _NEOS_LP_SIZE_WARN:
                    size_mb = size / (1024 * 1024)
                    diag_parts.append(
                        f"  ⚠ The LP file is {size_mb:.1f} MiB — consider a "
                        "local solver for large files."
                    )
            except OSError:
                pass
            return False, "\n".join(diag_parts)

        print(f"  Submitting to NEOS ({self.url}) …")
        job_number, password_or_err = self.submit_lp(lp_path, email, version=version)
        if job_number is None:
            return False, str(password_or_err)

        print(f"  NEOS job {job_number} submitted.  Waiting for results …")
        return self.wait_for_result(
            job_number, str(password_or_err), poll_interval=poll_interval
        )
