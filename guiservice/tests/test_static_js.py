"""Tests for guiservice static JavaScript behavior contracts."""

from pathlib import Path


def test_upload_case_has_no_success_alert_popup():
    """Uploading a case should not trigger a blocking success alert dialog."""
    app_js = Path(__file__).resolve().parent.parent / "static" / "js" / "app.js"
    content = app_js.read_text(encoding="utf-8")
    assert 'alert("Case loaded successfully!")' not in content
