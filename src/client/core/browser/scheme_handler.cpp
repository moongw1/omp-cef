#include "scheme_handler.hpp"
#include "system/resource_manager.hpp"
#include "include/wrapper/cef_helpers.h"
#include "include/cef_parser.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <cstring>

static const std::string& GetInternalLoadingHtml()
{
    static const std::string html = R"HTML(<!doctype html>
        <html>
            <head>
                <meta charset="utf-8" />
                <meta name="viewport" content="width=device-width, initial-scale=1" />
                <title>Downloading resources UI</title>
                <style>
                    html,body{margin:0;height:100%;font-family:system-ui,Segoe UI,Roboto,Arial;background:transparent;color:#e7eefc}
                    .wrap{height:100%;display:flex;align-items:center;justify-content:center}
                    .card{width:min(560px,92vw);padding:18px 18px 14px;border-radius:16px;background:#1a1a1a;box-shadow:0 10px 40px rgba(0,0,0,.35);backdrop-filter:blur(10px)}
                    .title{font-size:18px;font-weight:700;margin:0 0 10px}
                    .sub{opacity:.75;font-size:13px;margin:0 0 14px}
                    .bar{height:12px;border-radius:999px;background:rgba(255,255,255,.08);overflow:hidden}
                    .fill{height:100%;width:0%;background:#ff9933;}
                    .row{display:flex;justify-content:space-between;gap:12px;margin-top:12px;font-size:12px;opacity:.85}
                    .file{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:65%}
                </style>
            </head>
            <body>
                <div class="wrap"><div class="card">
                    <p class="title">Downloading ressources ...</p>
                    <p class="sub" id="sub">Loading ...</p>
                    <div class="bar"><div class="fill" id="fill"></div></div>
                    <div class="row"><div class="file" id="file">-</div><div id="pct">0%</div></div>
                    <div class="row justify-content-end"><div id="speed">0 KB/s</div></div>
                </div></div>
                <script>
                    const elFill=document.getElementById('fill'),elPct=document.getElementById('pct'),elSub=document.getElementById('sub'),elFile=document.getElementById('file'),elSpeed=document.getElementById('speed');
                    let total=0,got=0,targetPct=0,smoothPct=0,lastT=performance.now(),lastGot=0;
                    function fmtBytes(n){const u=["B","KB","MB","GB"];let i=0;while(n>=1024&&i<u.length-1){n/=1024;i++;}return `${n.toFixed(i?2:0)} ${u[i]}`;}
                    function tickSpeed(){const now=performance.now(),dt=(now-lastT)/1000;if(dt<=0.10)return;const dGot=got-lastGot,sp=dGot/dt;elSpeed.textContent=`${fmtBytes(sp)}/s`;lastT=now;lastGot=got;}
                    setInterval(tickSpeed,200);
                    function raf(){smoothPct+=(targetPct-smoothPct)*0.15;const pct=Math.max(0,Math.min(100,smoothPct));elFill.style.width=pct.toFixed(2)+'%';elPct.textContent=Math.round(pct)+'%';requestAnimationFrame(raf);}requestAnimationFrame(raf);
                    window.__ompcef={
                        manifest:(files,bytes)=>{total=bytes|0;elSub.textContent=`${files} fichier(s) • ${fmtBytes(total)}`;},
                        progress:(fileName,fileReceived,fileTotal,overallReceived,overallTotal)=>{got=overallReceived|0;total=overallTotal|0;targetPct=total?(got/total)*100:0;elFile.textContent=fileName||'—';elSub.textContent=`${fmtBytes(fileReceived)} / ${fmtBytes(fileTotal)} • Total ${fmtBytes(got)} / ${fmtBytes(total)}`;},
                        done:()=>{got=total;targetPct=100;elFile.textContent='Done';elSub.textContent='Loading ...';}
                    };
                </script>
            </body>
        </html>)HTML";
    return html;
}

static const std::string& GetInternalYouTubeHtml()
{
    static const std::string html = R"HTML(<!doctype html>
<html><head><meta charset="utf-8" /><meta name="viewport" content="width=device-width, initial-scale=1" /><meta name="referrer" content="origin"><title>YouTube</title><style>html,body{margin:0;height:100%;background:#000;overflow:hidden}iframe{border:0;width:100%;height:100%;display:block}</style></head>
<body><iframe id="yt" sandbox="allow-scripts allow-same-origin allow-presentation allow-popups allow-forms" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share" allowfullscreen referrerpolicy="origin"></iframe>
<script>(function(){const u=new URL(location.href),v=u.searchParams.get('v')||'';if(!v){document.body.innerHTML='<div style="color:#fff;padding:12px;font-family:system-ui">Missing video id</div>';return;}const autoplay=u.searchParams.get('autoplay')??'1',mute=u.searchParams.get('mute')??'1',controls=u.searchParams.get('controls')??'0',rel=u.searchParams.get('rel')??'0',start=u.searchParams.get('start'),end=u.searchParams.get('end'),p=new URLSearchParams();p.set('autoplay',autoplay);p.set('mute',mute);p.set('controls',controls);p.set('rel',rel);p.set('playsinline','1');p.set('iv_load_policy','3');p.set('modestbranding','1');p.set('enablejsapi','1');p.set('origin','http://cef');if(start)p.set('start',start);if(end)p.set('end',end);document.getElementById('yt').src=`https://www.youtube.com/embed/${encodeURIComponent(v)}?${p.toString()}`;})();</script></body></html>)HTML";
    return html;
}

static const std::string& GetInternalTwitchHtml()
{
    static const std::string html = R"HTML(<!doctype html>
<html><head><meta charset="utf-8" /><meta name="viewport" content="width=device-width, initial-scale=1" /><meta name="referrer" content="origin"><title>Twitch</title><style>html,body{margin:0;height:100%;background:#000;overflow:hidden}iframe{border:0;width:100%;height:100%;display:block}</style></head>
<body><iframe id="tw" sandbox="allow-scripts allow-same-origin allow-presentation allow-popups allow-forms" allow="autoplay; fullscreen; picture-in-picture; encrypted-media" allowfullscreen referrerpolicy="origin"></iframe>
<script>(function(){const u=new URL(location.href),params=new URLSearchParams(u.search),hostname=location.hostname||'cef';params.set('parent',hostname);const channel=params.get('channel'),video=params.get('video'),clip=params.get('clip');if(!channel&&!video&&!clip){document.body.innerHTML='<div style="color:#fff;padding:12px;font-family:system-ui">Missing Twitch channel/video/clip</div>';return;}const finalParams=new URLSearchParams();if(channel)finalParams.set('channel',channel);if(video)finalParams.set('video',video);if(clip)finalParams.set('collection',clip);for(const[k,v]of params.entries()){if(k!=='channel'&&k!=='video'&&k!=='clip')finalParams.set(k,v);}finalParams.set('parent',hostname);finalParams.set('autoplay','true');finalParams.set('muted','false');document.getElementById('tw').src='https://player.twitch.tv/?'+finalParams.toString();})();</script></body></html>)HTML";
    return html;
}

LocalSchemeHandlerFactory::LocalSchemeHandlerFactory(ResourceManager& resource_manager)
    : resource_manager_(resource_manager)
{
}

CefRefPtr<CefResourceHandler> LocalSchemeHandlerFactory::Create(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& scheme_name,
    CefRefPtr<CefRequest> request)
{
    CEF_REQUIRE_IO_THREAD();
    return new LocalResourceHandler(resource_manager_);
}

static const std::unordered_map<std::string, std::string>& GetMimeTypeMap()
{
    static const std::unordered_map<std::string, std::string> mime_map = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
        {".js", "application/javascript"}, {".mjs", "application/javascript"},
        {".json", "application/json"}, {".xml", "application/xml"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
        {".webp", "image/webp"}, {".bmp", "image/bmp"},
        {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"},
        {".m4a", "audio/mp4"}, {".aac", "audio/aac"}, {".flac", "audio/flac"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".ogv", "video/ogg"},
        {".avi", "video/x-msvideo"}, {".woff", "font/woff"}, {".woff2", "font/woff2"},
        {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".eot", "application/vnd.ms-fontobject"},
        {".pdf", "application/pdf"}, {".txt", "text/plain"}, {".md", "text/markdown"},
    };
    return mime_map;
}

static std::string GetMimeType(const std::string& path)
{
    const size_t pos = path.rfind('.');
    if (pos == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto& mime_map = GetMimeTypeMap();
    const auto it = mime_map.find(ext);
    return it != mime_map.end() ? it->second : "application/octet-stream";
}

LocalResourceHandler::LocalResourceHandler(ResourceManager& resource_manager)
    : resource_manager_(resource_manager), read_offset_(0)
{
}

bool LocalResourceHandler::ProcessRequest(
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> callback)
{
    CEF_REQUIRE_IO_THREAD();

    data_.clear();
    shared_data_.reset();
    read_offset_ = 0;

    std::string url = request->GetURL();
    std::string path_part;

    if (url.rfind("cef://", 0) == 0)
        path_part = url.substr(6);
    else if (url.rfind("http://cef/", 0) == 0)
        path_part = url.substr(11);
    else
        return false;

    const size_t query_pos = path_part.find('?');
    if (query_pos != std::string::npos)
        path_part.resize(query_pos);

    const size_t fragment_pos = path_part.find('#');
    if (fragment_pos != std::string::npos)
        path_part.resize(fragment_pos);

    const size_t first_slash_pos = path_part.find('/');
    if (first_slash_pos == std::string::npos)
        return false;

    const std::string resource_name = path_part.substr(0, first_slash_pos);
    std::string internal_path = path_part.substr(first_slash_pos + 1);

    if (resource_name == "__internal")
    {
        const std::string* html = nullptr;
        if (internal_path == "loading.html") html = &GetInternalLoadingHtml();
        else if (internal_path == "youtube.html") html = &GetInternalYouTubeHtml();
        else if (internal_path == "twitch.html") html = &GetInternalTwitchHtml();

        if (html)
        {
            data_.assign(html->begin(), html->end());
            mime_type_ = "text/html";
            callback->Continue();
            return true;
        }
    }

    const size_t pos = internal_path.rfind('.');
    if (pos == std::string::npos)
        return false;

    std::string ext = internal_path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (GetMimeTypeMap().find(ext) == GetMimeTypeMap().end())
        return false;

    CefString decoded = CefURIDecode(
        internal_path,
        true,
        static_cast<cef_uri_unescape_rule_t>(UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS));

    internal_path = decoded.ToString();
    std::replace(internal_path.begin(), internal_path.end(), '\\', '/');
    if (internal_path.find("..") != std::string::npos || internal_path.find("~") != std::string::npos)
        return false;

    shared_data_ = resource_manager_.GetFileContentShared(resource_name, internal_path);
    if (!shared_data_)
        return false;

    mime_type_ = GetMimeType(internal_path);
    callback->Continue();
    return true;
}

void LocalResourceHandler::GetResponseHeaders(
    CefRefPtr<CefResponse> response,
    int64_t& response_length,
    CefString& redirectUrl)
{
    CEF_REQUIRE_IO_THREAD();

    response->SetMimeType(mime_type_);
    response->SetStatus(200);

    // Let Chromium cache static local assets for the lifetime of this session.
    // The VFS is replaced when the server resource changes, so every new browser
    // request sees the current immutable buffer.
    CefResponse::HeaderMap headers;
    response->GetHeaderMap(headers);
    headers.insert({"Cache-Control", "private, max-age=300"});
    response->SetHeaderMap(headers);

    response_length = static_cast<int64_t>(shared_data_ ? shared_data_->size() : data_.size());
}

bool LocalResourceHandler::ReadResponse(
    void* data_out,
    int bytes_to_read,
    int& bytes_read,
    CefRefPtr<CefCallback> callback)
{
    CEF_REQUIRE_IO_THREAD();

    const std::vector<uint8_t>& source = shared_data_ ? *shared_data_ : data_;
    if (read_offset_ >= source.size())
    {
        bytes_read = 0;
        return false;
    }

    const size_t remaining_bytes = source.size() - read_offset_;
    const size_t bytes_to_copy = std::min(static_cast<size_t>(bytes_to_read), remaining_bytes);
    std::memcpy(data_out, source.data() + read_offset_, bytes_to_copy);

    read_offset_ += bytes_to_copy;
    bytes_read = static_cast<int>(bytes_to_copy);
    return true;
}

void LocalResourceHandler::Cancel()
{
    CEF_REQUIRE_IO_THREAD();

    // Keep vector capacity for reuse; shrink_to_fit here caused repeated heap
    // allocations when pages rapidly request/release many local assets.
    data_.clear();
    shared_data_.reset();
    read_offset_ = 0;
}
