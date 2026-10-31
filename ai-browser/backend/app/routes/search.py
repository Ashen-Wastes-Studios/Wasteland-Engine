from fastapi import APIRouter, Query
from pydantic import BaseModel
from app.services.search import search_web
from app.services.ai import generate_overview

router = APIRouter()

class SearchResponse(BaseModel):
    query: str
    ai_overview: str | None
    results: list[dict]

@router.get("/search", response_model=SearchResponse)
async def search_web_endpoint(q: str = Query(...), ai_overview: bool = Query(True)):
    data = await search_web(q, ai_overview=ai_overview)
    overview = None
    if ai_overview and data["results"]:
        try:
            overview = await generate_overview(q, data["results"])
        except Exception:
            overview = None
    return SearchResponse(query=data["query"], ai_overview=overview, results=data["results"])
