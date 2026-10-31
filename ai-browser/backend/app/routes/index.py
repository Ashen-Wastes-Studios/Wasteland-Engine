from fastapi import APIRouter, BackgroundTasks, Query
from pydantic import BaseModel
from app.services.crawler import crawl_url
from app.services.indexer import index_document, search_index

router = APIRouter()

class CrawlRequest(BaseModel):
    url: str
    depth: int = 1

class CrawlResponse(BaseModel):
    job_id: str
    status: str

class IndexSearchResponse(BaseModel):
    query: str
    results: list[dict]

@router.post("/index/crawl", response_model=CrawlResponse)
async def crawl_and_index(body: CrawlRequest, background_tasks: BackgroundTasks):
    async def job():
        doc = await crawl_url(body.url)
        await index_document(doc)

    background_tasks.add_task(job)
    return CrawlResponse(job_id="stub", status="queued")

@router.get("/index/search", response_model=IndexSearchResponse)
async def search_index_endpoint(q: str = Query(...), limit: int = Query(10)):
    results = await search_index(q, limit=limit)
    return IndexSearchResponse(query=q, results=results)
