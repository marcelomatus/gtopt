"use client";

import { useState, useCallback, useEffect, FormEvent } from "react";

interface JobInfo {
  token: string;
  status: "pending" | "running" | "completed" | "failed";
  createdAt: string;
  completedAt?: string;
  systemFile: string;
  error?: string;
}

export default function Home() {
  const [file, setFile] = useState<File | null>(null);
  const [systemFile, setSystemFile] = useState("system_c0.json");
  const [submitting, setSubmitting] = useState(false);
  const [message, setMessage] = useState<{
    type: "error" | "success" | "info";
    text: string;
  } | null>(null);
  const [submittedToken, setSubmittedToken] = useState<string | null>(null);

  const [lookupToken, setLookupToken] = useState("");
  const [lookupResult, setLookupResult] = useState<JobInfo | null>(null);
  const [lookupError, setLookupError] = useState<string | null>(null);

  const [recentJobs, setRecentJobs] = useState<JobInfo[]>([]);

  const fetchJobs = useCallback(async () => {
    try {
      const res = await fetch("/api/jobs");
      if (res.ok) {
        const data = await res.json();
        setRecentJobs(data.jobs || []);
      }
    } catch {
      // Ignore fetch errors
    }
  }, []);

  useEffect(() => {
    fetchJobs();
    const interval = setInterval(fetchJobs, 5000);
    return () => clearInterval(interval);
  }, [fetchJobs]);

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    if (!file || !systemFile) return;

    setSubmitting(true);
    setMessage(null);
    setSubmittedToken(null);

    try {
      const formData = new FormData();
      formData.append("file", file);
      formData.append("systemFile", systemFile);

      const res = await fetch("/api/jobs", {
        method: "POST",
        body: formData,
      });

      const data = await res.json();

      if (res.ok) {
        setSubmittedToken(data.token);
        setMessage({ type: "success", text: data.message });
        fetchJobs();
      } else {
        setMessage({ type: "error", text: data.error || "Submission failed" });
      }
    } catch (err) {
      setMessage({
        type: "error",
        text: `Network error: ${err instanceof Error ? err.message : String(err)}`,
      });
    } finally {
      setSubmitting(false);
    }
  }

  async function handleLookup() {
    if (!lookupToken.trim()) return;

    setLookupResult(null);
    setLookupError(null);

    try {
      const res = await fetch(`/api/jobs/${lookupToken.trim()}`);
      if (res.ok) {
        const data = await res.json();
        setLookupResult(data);
      } else {
        const data = await res.json();
        setLookupError(data.error || "Job not found");
      }
    } catch {
      setLookupError("Network error");
    }
  }

  function statusClass(status: string) {
    return `status-badge status-${status}`;
  }

  return (
    <div className="container">
      <header>
        <h1>⚡ gtopt Web Service</h1>
        <p>Upload optimization cases, run gtopt, and download results</p>
      </header>

      {/* Submit Job */}
      <div className="card">
        <h2>Submit a New Job</h2>
        <form onSubmit={handleSubmit}>
          <div className="form-group">
            <label htmlFor="file">Case Archive (zip)</label>
            <input
              id="file"
              type="file"
              accept=".zip"
              onChange={(e) => setFile(e.target.files?.[0] || null)}
            />
            <p className="hint">
              Upload a .zip file containing the case directory (e.g., the
              contents of cases/c0 including system_c0.json and the system_c0/
              data directory)
            </p>
          </div>
          <div className="form-group">
            <label htmlFor="systemFile">System JSON Filename</label>
            <input
              id="systemFile"
              type="text"
              value={systemFile}
              onChange={(e) => setSystemFile(e.target.value)}
              placeholder="e.g. system_c0.json"
            />
            <p className="hint">
              The name of the system JSON file inside the archive
            </p>
          </div>
          <button
            type="submit"
            className="btn btn-primary"
            disabled={submitting || !file}
          >
            {submitting ? "Submitting..." : "Submit Job"}
          </button>
        </form>

        {message && (
          <div className={`alert alert-${message.type}`} style={{ marginTop: "1rem" }}>
            {message.text}
          </div>
        )}

        {submittedToken && (
          <div style={{ marginTop: "1rem" }}>
            <strong>Your job token:</strong>
            <div className="token-display">{submittedToken}</div>
            <p className="hint">
              Save this token to check status and download results later.
            </p>
          </div>
        )}
      </div>

      {/* Lookup Job */}
      <div className="card">
        <h2>Check Job Status</h2>
        <div className="form-group">
          <label htmlFor="lookupToken">Job Token</label>
          <div className="lookup-row">
            <input
              id="lookupToken"
              type="text"
              value={lookupToken}
              onChange={(e) => setLookupToken(e.target.value)}
              placeholder="Enter job token..."
              onKeyDown={(e) => e.key === "Enter" && handleLookup()}
            />
            <button className="btn btn-primary" onClick={handleLookup}>
              Check
            </button>
          </div>
        </div>

        {lookupError && (
          <div className="alert alert-error">{lookupError}</div>
        )}

        {lookupResult && (
          <div className="alert alert-info">
            <p>
              <strong>Status:</strong>{" "}
              <span className={statusClass(lookupResult.status)}>
                {lookupResult.status}
              </span>
            </p>
            <p>
              <strong>System File:</strong> {lookupResult.systemFile}
            </p>
            <p>
              <strong>Created:</strong>{" "}
              {new Date(lookupResult.createdAt).toLocaleString()}
            </p>
            {lookupResult.completedAt && (
              <p>
                <strong>Completed:</strong>{" "}
                {new Date(lookupResult.completedAt).toLocaleString()}
              </p>
            )}
            {lookupResult.error && (
              <p>
                <strong>Error:</strong> {lookupResult.error}
              </p>
            )}
            {(lookupResult.status === "completed" ||
              lookupResult.status === "failed") && (
              <a
                href={`/api/jobs/${lookupResult.token}/download`}
                className="btn btn-success"
                style={{ marginTop: "0.5rem" }}
              >
                Download Results
              </a>
            )}
          </div>
        )}
      </div>

      <hr className="section-divider" />

      {/* Recent Jobs */}
      <div className="card">
        <h2>Recent Jobs</h2>
        {recentJobs.length === 0 ? (
          <p style={{ color: "var(--muted)" }}>No jobs submitted yet.</p>
        ) : (
          <ul className="job-list">
            {recentJobs.map((job) => (
              <li key={job.token} className="job-item">
                <div className="job-info">
                  <div>
                    <strong>{job.systemFile}</strong>
                  </div>
                  <div className="token">{job.token}</div>
                  <div style={{ fontSize: "0.85rem", color: "var(--muted)" }}>
                    {new Date(job.createdAt).toLocaleString()}
                  </div>
                </div>
                <div className="job-actions">
                  <span className={statusClass(job.status)}>{job.status}</span>
                  {(job.status === "completed" || job.status === "failed") && (
                    <a
                      href={`/api/jobs/${job.token}/download`}
                      className="btn btn-success"
                      style={{ fontSize: "0.85rem", padding: "0.3rem 0.75rem" }}
                    >
                      Download
                    </a>
                  )}
                </div>
              </li>
            ))}
          </ul>
        )}
      </div>

      <footer>
        <p>gtopt Web Service — Generation &amp; Transmission Optimization</p>
      </footer>
    </div>
  );
}
