# SPDX-License-Identifier: BSD-3-Clause
"""NEOS optimization server client for gtopt_check_lp.

Submits infeasible LP files to the NEOS CPLEX LP solver via its XML-RPC API
and waits for the conflict-analysis results.

CPLEX Interactive Optimizer command reference (v20.1)
-----------------------------------------------------
After an infeasible LP solve the correct command sequence to identify the
minimal infeasible subsystem is:

1. ``set preprocessing presolve 0``   – disable presolve so that infeasibility
   is detected by the simplex method rather than presolve; without this step
   the conflict refiner has no LP basis to work from.
2. ``optimize``                        – re-solve; simplex will confirm
   infeasibility and produce a dual infeasibility certificate.
3. ``refineconflict``                  – invoke the CPLEX conflict refiner
   (the old ``conflict`` command was removed in newer CPLEX versions).
4. ``display conflict all``            – print the minimal conflict set
   (infeasible constraints and bound members).

``conflict`` is **not** a valid CPLEX interactive command in v20.1+; using it
produces "Command 'conflict' does not exist."  Always use ``refineconflict``.
"""

import http.client
import time
import xmlrpc.client
from pathlib import Path
from typing import Optional

from ._compress import read_lp_text

_NEOS_DEFAULT_URL = "https://neos-server.org:3333"

# NEOS CPLEX LP XML template.
#
# The <post> section is executed *after* the initial LP read+solve.  Because
# NEOS typically runs with presolve on, the initial solve may terminate via
# presolve before the conflict refiner state is available.  We therefore:
#   1. Disable presolve.
#   2. Re-solve (simplex now confirms infeasibility).
#   3. Run the conflict refiner with `refineconflict`.
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
set preprocessing presolve 0
optimize
refineconflict
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
            return None, f"Network error connecting to {self.url}: {exc}"

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
            return (
                False,
                f"Cannot reach NEOS server at {self.url}. "
                "Check your internet connection.",
            )

        print(f"  Submitting to NEOS ({self.url}) …")
        job_number, password_or_err = self.submit_lp(lp_path, email, version=version)
        if job_number is None:
            return False, str(password_or_err)

        print(f"  NEOS job {job_number} submitted.  Waiting for results …")
        return self.wait_for_result(
            job_number, str(password_or_err), poll_interval=poll_interval
        )
