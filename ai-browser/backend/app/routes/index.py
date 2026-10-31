from fastapi import APIRouter, BackgroundTasks
from pydantic import BaseModel
from app.services.crawler import crawl_url
from app.services.indexer import index_document

router = APIRouter()

class CrawlRequest(BaseModel):
    url: str
    depth: int = 1

class CrawlResponse(BaseModel):
    job_id: str
    status: str

@router.post("/index/crawl", response_model=CrawlResponse)
async def crawl_and_index(body: CrawlRequest, background_tasks: BackgroundTasks):
    async def job():
        doc = await crawl_url(body.url)
        await index_document(doc)

    background_tasks.add_task(job)
    return CrawlResponse(job_id="stub", status="queued")
