import hmac
import logging
import re
import time
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from typing import Annotated, Literal
from uuid import uuid4

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, ConfigDict, StringConstraints, model_validator

from .reader import MAX_IMAGE_EDGE, MediaCache, Reader, ReaderError


LOGGER = logging.getLogger("xhs_reader")
UI = Path(__file__).parent / "ui"
JOB_LIMIT = 128
PENDING_LIMIT = 8
JOB_AGE_SECONDS = 900
ID = Annotated[str, StringConstraints(pattern=r"^[0-9a-zA-Z_-]{1,64}$")]


class JobRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    request_id: ID
    action: Literal["open", "more", "refresh", "detail"]
    session_id: ID | None = None
    note_id: ID | None = None

    @model_validator(mode="after")
    def validate_action(self):
        if self.action in {"more", "detail"} and not self.session_id:
            raise ValueError("session_id is required for more and detail")
        if (self.action == "detail") != (self.note_id is not None):
            raise ValueError("note_id must be supplied only for detail")
        return self


@dataclass
class Job:
    request: JobRequest
    id: str
    status: str = "pending"
    result: dict | None = None
    error: str | None = None
    finished: float | None = None


class Jobs:
    def __init__(self, reader):
        self.reader = reader
        self.pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix="xhs-browser")
        self.lock = Lock()
        self.entries = OrderedDict()
        self.requests = {}

    def _prune(self):
        now = time.monotonic()
        completed = [(job_id, job) for job_id, job in self.entries.items() if job.finished is not None]
        for job_id, job in completed:
            if len(self.entries) < JOB_LIMIT and now - job.finished < JOB_AGE_SECONDS:
                continue
            del self.entries[job_id]
            del self.requests[job.request.request_id]

    def submit(self, request):
        with self.lock:
            self._prune()
            existing = self.requests.get(request.request_id)
            if existing is not None:
                if existing.request != request:
                    raise HTTPException(409, "request_id_conflict")
                return existing.id
            active = sum(job.finished is None for job in self.entries.values())
            if active >= PENDING_LIMIT:
                raise HTTPException(429, "queue_full")
            job = Job(request, uuid4().hex)
            self.entries[job.id] = job
            self.requests[request.request_id] = job
            self.pool.submit(self._run, job)
            return job.id

    def _run(self, job):
        with self.lock:
            job.status = "running"
        result = error = None
        try:
            result = self.reader.perform(job.request)
        except ReaderError as exception:
            error = exception.code
        except Exception as exception:
            LOGGER.error("Reader action %s failed (%s)", job.request.action, type(exception).__name__)
            error = "internal_error"
        with self.lock:
            job.result = result
            job.error = error
            job.status = "error" if error else "done"
            job.finished = time.monotonic()

    def get(self, job_id):
        with self.lock:
            self._prune()
            job = self.entries.get(job_id)
            if job is None:
                raise HTTPException(404, "job_missing")
            response = {"job_id": job_id, "status": job.status}
            if job.result is not None:
                response["result"] = job.result
            if job.error is not None:
                response["error"] = job.error
            return response

    def close(self):
        try:
            self.pool.submit(self.reader.source.close).result()
        finally:
            self.pool.shutdown(cancel_futures=True)


def create_app(source, data_dir, token=None):
    media = MediaCache(data_dir / source.mode / "media", demo=source.mode == "demo")

    @asynccontextmanager
    async def lifespan(app):
        jobs = Jobs(Reader(source, media))
        app.state.jobs = jobs
        try:
            yield
        finally:
            jobs.close()

    app = FastAPI(title="小红书阅读服务", docs_url=None, redoc_url=None, openapi_url=None, lifespan=lifespan)

    @app.middleware("http")
    async def headers(request: Request, call_next):
        length = request.headers.get("content-length")
        if request.method == "POST" and (length is None or not length.isdecimal() or int(length) > 4096):
            return JSONResponse({"detail": "invalid_content_length"}, status_code=413)
        response = await call_next(request)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; img-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'"
        )
        return response

    def require_token(authorization: Annotated[str | None, Header()] = None):
        if token is None:
            return
        scheme, _, supplied = (authorization or "").partition(" ")
        if scheme.lower() != "bearer" or not hmac.compare_digest(supplied.encode(), token.encode()):
            raise HTTPException(401, "invalid_token", headers={"WWW-Authenticate": "Bearer"})

    @app.get("/", include_in_schema=False)
    def home():
        return FileResponse(UI / "index.html", media_type="text/html")

    @app.get("/ui/{name}", include_in_schema=False)
    def ui_asset(name: Literal["app.js", "style.css"]):
        content_type = "text/javascript" if name == "app.js" else "text/css"
        return FileResponse(UI / name, media_type=content_type)

    @app.get("/v1/health")
    def health():
        return {"status": "ok", "mode": source.mode, "token_required": token is not None}

    @app.post("/v1/jobs", status_code=202, dependencies=[Depends(require_token)])
    def submit(request: JobRequest):
        return {"job_id": app.state.jobs.submit(request)}

    @app.get("/v1/jobs/{job_id}", dependencies=[Depends(require_token)])
    def job_status(job_id: ID):
        return app.state.jobs.get(job_id)

    def media_file(image_id, gray4_size=None):
        if not re.fullmatch(r"[a-f0-9]{64}", image_id):
            raise HTTPException(404, "image_missing")
        try:
            return media.get(image_id, gray4_size)
        except ReaderError as error:
            raise HTTPException(404 if error.code == "image_missing" else 502, error.code) from error

    @app.get("/v1/media/{image_id}.jpg", dependencies=[Depends(require_token)])
    def image(image_id: str):
        return FileResponse(media_file(image_id), media_type="image/jpeg")

    @app.get("/v1/media/{image_id}.gray4", dependencies=[Depends(require_token)])
    def image_gray4(
        image_id: str,
        width: Annotated[int, Query(ge=1, le=MAX_IMAGE_EDGE)] = 684,
        height: Annotated[int, Query(ge=1, le=MAX_IMAGE_EDGE)] = 1216,
    ):
        return FileResponse(media_file(image_id, (width, height)), media_type="application/octet-stream")

    return app
