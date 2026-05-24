/*
 * app.c — MediaLog: Personal Media Tracker (XP Edition)
 * Build: see Makefile  |  Run: make serve
 */

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── HTML BUFFER ─────────────────────────────────── */
#define HMAX (512 * 1024)
static char   g_html[HMAX];
static size_t g_hlen = 0;

static void h_reset(void) { g_html[0]='\0'; g_hlen=0; }
static void h_cat(const char *s) {
    size_t n=strlen(s);
    if (g_hlen+n<HMAX-1){memcpy(g_html+g_hlen,s,n+1);g_hlen+=n;}
}
static void h_fmt(const char *fmt,...) {
    char tmp[16384]; va_list ap;
    va_start(ap,fmt); vsnprintf(tmp,sizeof tmp,fmt,ap); va_end(ap);
    h_cat(tmp);
}

/* ── APP STATE ───────────────────────────────────── */
typedef enum { VIEW_LIST, VIEW_ADD, VIEW_DETAIL, VIEW_STATS } AppView;
static AppView g_view       = VIEW_LIST;
static char    g_filter[32]  = "all";
static char    g_add_type[32] = "movie";
static int     g_detail_id   = 0;
static char    g_search[256]  = "";

/* ── JS BRIDGE ───────────────────────────────────── */
EM_JS(void, js_render, (const char *ptr), {
    var activeId  = document.activeElement ? document.activeElement.id : null;
    var activeSel = (document.activeElement && document.activeElement.selectionStart !== undefined)
                    ? document.activeElement.selectionStart : null;
    document.getElementById('app').innerHTML = UTF8ToString(ptr);
    window.scrollTo(0,0);
    if (activeId) {
        var el = document.getElementById(activeId);
        if (el) {
            el.focus();
            if (activeSel !== null && el.setSelectionRange) {
                el.setSelectionRange(activeSel, activeSel);
            }
        }
    }
});
EM_JS(void, js_db_exec, (const char *ptr), {
    try { Module._db.run(UTF8ToString(ptr)); }
    catch(e) { console.error('[exec]',e.message); }
});
EM_JS(void, js_db_query, (const char *ptr), {
    Module._rows=[];
    try {
        const stmt=Module._db.prepare(UTF8ToString(ptr));
        while(stmt.step()) Module._rows.push(stmt.getAsObject());
        stmt.free();
    } catch(e) { console.error('[query]',e.message); }
});
EM_JS(int,    js_row_count, (), { return Module._rows?Module._rows.length:0; });
EM_JS(char*, js_get_str, (int row, const char *col), {
    var c=UTF8ToString(col), obj=Module._rows[row];
    var v=(obj&&c in obj&&obj[c]!==null&&obj[c]!==undefined)?String(obj[c]):"";
    var len=lengthBytesUTF8(v)+1, buf=_malloc(len);
    stringToUTF8(v,buf,len); return buf;
});
EM_JS(int,    js_get_int,   (int row, const char *col), {
    const c=UTF8ToString(col), obj=Module._rows[row];
    const v=(obj&&c in obj)?obj[c]:null;
    return (v===null||v===undefined)?0:(parseInt(v)|0);
});
EM_JS(double, js_get_float, (int row, const char *col), {
    const c=UTF8ToString(col), obj=Module._rows[row];
    const v=(obj&&c in obj)?obj[c]:null;
    return (v===null||v===undefined)?0.0:parseFloat(v);
});

EM_JS(void, js_init_db, (), {
    Module._db = new Module._SQL.Database();
    Module._db.run("PRAGMA foreign_keys = ON;");
    Module._db.run(
        "CREATE TABLE IF NOT EXISTS media (" +
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT," +
        "  media_type   TEXT    NOT NULL," +
        "  title        TEXT    NOT NULL," +
        "  description  TEXT," +
        "  release_year INTEGER," +
        "  poster_url   TEXT," +
        "  director     TEXT," +
        "  cast_list    TEXT," +
        "  created_at   DATETIME DEFAULT CURRENT_TIMESTAMP" +
        ");"
    );
    Module._db.run(
        "CREATE TABLE IF NOT EXISTS user_media_experience (" +
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT," +
        "  media_id      INTEGER NOT NULL," +
        "  status        TEXT    DEFAULT 'want'," +
        "  rating        REAL," +
        "  notes         TEXT," +
        "  rewatch_count INTEGER DEFAULT 0," +
        "  favorite      INTEGER DEFAULT 0," +
        "  created_at    DATETIME DEFAULT CURRENT_TIMESTAMP," +
        "  FOREIGN KEY(media_id) REFERENCES media(id) ON DELETE CASCADE" +
        ");"
    );
    Module._db.run("CREATE INDEX IF NOT EXISTS idx_type  ON media(media_type);");
    Module._db.run("CREATE INDEX IF NOT EXISTS idx_title ON media(title);");

    var n = Module._db.exec("SELECT COUNT(*) AS n FROM media;")[0].values[0][0];
    if (n === 0) {
        var seed = [
            ["movie","Dune: Part Two",2024,"completed",4.5,
             "An epic continuation of the Arrakis saga.",
             "Denis Villeneuve","Timothee Chalamet, Zendaya, Rebecca Ferguson",
             "https://image.tmdb.org/t/p/w342/1pdfLvkbY9ohJlCjQH2CZjjYVvJ.jpg"],
            ["book","Project Hail Mary",2021,"completed",5,
             "A lone astronaut must save Earth from an extinction-level threat.",
             "Andy Weir","",
             "https://covers.openlibrary.org/b/isbn/0593135202-M.jpg"],
            ["tv","Severance",2022,"watching",4,
             "Employees undergo a procedure to separate work and personal memories.",
             "Ben Stiller","Adam Scott, Patricia Arquette, John Turturro",""],
            ["album","Bright Future",2024,"completed",4.5,
             "Adrianne Lenker's intimate acoustic record.",
             "Adrianne Lenker","",""],
            ["game","Hollow Knight",2017,"want",0,
             "An epic action-adventure through a vast ruined underground kingdom.",
             "Team Cherry","",""]
        ];
        for (var i = 0; i < seed.length; i++) {
            var row = seed[i];
            var type=row[0], title=row[1], year=row[2], status=row[3];
            var rating=row[4], notes=row[5], director=row[6], cast=row[7], poster=row[8];
            Module._db.run(
                "INSERT INTO media(media_type,title,release_year,description,poster_url,director,cast_list)" +
                " VALUES(?,?,?,?,?,?,?);",
                [type, title, year, notes, poster, director, cast]
            );
            var id = Module._db.exec("SELECT last_insert_rowid() AS id;")[0].values[0][0];
            Module._db.run(
                "INSERT INTO user_media_experience(media_id,status,rating,notes,favorite)" +
                " VALUES(?,?,?,?,?);",
                [id, status, rating || null, notes, 0]
            );
        }
    }
    Module._app_start();
});

EM_JS(int, js_confirm, (const char *ptr), {
    return window.confirm(UTF8ToString(ptr))?1:0;
});

/* ── HELPERS ─────────────────────────────────────── */
static const char *type_icon(const char *t) {
    if (!t||!*t)            return "▣";
    if (!strcmp(t,"movie")) return "⬡";
    if (!strcmp(t,"book"))  return "◈";
    if (!strcmp(t,"tv"))    return "◫";
    if (!strcmp(t,"album")) return "◎";
    if (!strcmp(t,"game"))  return "◆";
    return "▣";
}
static const char *status_label(const char *s) {
    if (!s||!*s)                return "–";
    if (!strcmp(s,"completed")) return "Completed";
    if (!strcmp(s,"watching"))  return "In Progress";
    if (!strcmp(s,"want"))      return "Want to";
    if (!strcmp(s,"dropped"))   return "Dropped";
    return s;
}
static void sql_esc(const char *src, char *dst, size_t max) {
    size_t j=0;
    for (size_t i=0; src&&src[i]&&j+2<max; i++) {
        if (src[i]=='\'') dst[j++]='\'';
        dst[j++]=src[i];
    }
    dst[j]='\0';
}
static void make_stars(char *buf, double r, int max) {
    buf[0]='\0';
    int full=(int)(r+0.25);
    for (int i=1; i<=max; i++) strcat(buf, i<=full?"★":"☆");
}

/* ── TOOLBAR ─────────────────────────────────────── */
static void render_toolbar(void) {
    h_cat(
        "<div class='xp-toolbar'>"
        "<button class='tb-btn' onclick=\"C.nav('list','all')\">⊞ All Media</button>"
        "<div class='xp-sep'></div>"
    );
    struct { const char *id,*icon,*lbl; } tabs[]={
        {"movie","⬡","Films"},{"book","◈","Books"},
        {"tv","◫","TV"},{"album","◎","Music"},{"game","◆","Games"}
    };
    for (int i=0;i<5;i++) {
        int a=!strcmp(g_filter,tabs[i].id)&&g_view==VIEW_LIST;
        h_fmt("<button class='tb-btn%s' onclick=\"C.nav('list','%s')\">%s %s</button>",
            a?" active":"",tabs[i].id,tabs[i].icon,tabs[i].lbl);
    }
    h_cat("<div class='xp-sep'></div>");
    h_fmt("<button class='tb-btn%s' onclick=\"C.nav('stats','')\">📊 Stats</button>",
        g_view==VIEW_STATS?" active":"");
    h_fmt("<button class='tb-btn%s add-btn' onclick=\"C.nav('add','')\">+ Add Entry</button>",
        g_view==VIEW_ADD?" active":"");
    h_cat("</div>");
    h_fmt(
        "<div class='xp-address'>"
        "<span class='addr-lbl'>Search:</span>"
        "<input class='addr-box' id='search-box' value='%s' "
        "placeholder='Search titles...' oninput=\"C.search(this.value)\">"
        "</div>",
        g_search
    );
}

/* ── LIST VIEW ───────────────────────────────────── */
static void render_list(void) {
    char where[1024]="";
    if (g_search[0]) {
        char esc[512]; sql_esc(g_search,esc,sizeof esc);
        if (!strcmp(g_filter,"all")||!g_filter[0])
            snprintf(where,sizeof where,"WHERE m.title LIKE '%%%s%%'",esc);
        else
            snprintf(where,sizeof where,
                "WHERE m.media_type='%s' AND m.title LIKE '%%%s%%'",g_filter,esc);
    } else if (g_filter[0]&&strcmp(g_filter,"all")) {
        snprintf(where,sizeof where,"WHERE m.media_type='%s'",g_filter);
    }

    char sql[1024];
    snprintf(sql,sizeof sql,
        "SELECT m.id,m.media_type,m.title,m.release_year,"
        "m.poster_url,e.status,e.rating,e.favorite"
        " FROM media m JOIN user_media_experience e ON e.media_id=m.id"
        " %s ORDER BY m.created_at DESC;", where);
    js_db_query(sql);
    int n=js_row_count();

    h_reset(); render_toolbar();
    h_cat("<div class='xp-main'>");

    if (n==0) {
        h_cat(
            "<div class='xp-empty'>"
            "<div class='xp-empty-icon'>◈</div>"
            "<div class='xp-empty-text'>No entries found.<br>"
            "Click <b>+ Add Entry</b> in the toolbar to get started.</div>"
            "</div>"
        );
    } else {
        h_cat(
            "<table class='xp-list'>"
            "<thead><tr>"
            "<th style='width:44px'>Cover</th>"
            "<th>Title</th><th>Type</th><th>Year</th>"
            "<th>Status</th><th>Rating</th><th style='width:36px'></th>"
            "</tr></thead><tbody>"
        );
        for (int i=0;i<n;i++) {
            int    id     = js_get_int(i,"id");
            char  *type   = js_get_str(i,"media_type");
            char  *title  = js_get_str(i,"title");
            int    year   = js_get_int(i,"release_year");
            char  *poster = js_get_str(i,"poster_url");
            char  *status = js_get_str(i,"status");
            double rating = js_get_float(i,"rating");
            int    fav    = js_get_int(i,"favorite");
            char   stars[64]=""; make_stars(stars,rating,5);

            h_cat("<tr>");
            h_cat("<td class='col-poster'>");
            if (poster&&*poster)
                h_fmt("<img class='poster-thumb' src='%s' loading='lazy'>",poster);
            else
                h_fmt("<div class='poster-ph'>%s</div>",type_icon(type));
            h_cat("</td>");
            h_fmt("<td class='col-title' onclick='C.detail(%d)'>%s%s</td>",
                id, title, fav?" <span class='fav-mark'>♥</span>":"");
            h_fmt("<td><span class='type-badge type-%s'>%s %s</span></td>",
                type,type_icon(type),type);
            if (year>0) h_fmt("<td>%d</td>",year);
            else h_cat("<td class='dim'>–</td>");
            h_fmt("<td><span class='status-badge s-%s'>%s</span></td>",
                status,status_label(status));
            if (rating>=0.5) h_fmt("<td class='stars'>%s</td>",stars);
            else h_cat("<td class='dim'>–</td>");
            h_fmt("<td><button class='del-btn' "
                "onclick='event.stopPropagation();C.del(%d)'>✕</button></td>",id);
            h_cat("</tr>");

            free(type);free(title);free(poster);free(status);
        }
        h_cat("</tbody></table>");
    }

    h_cat("</div>");
    h_fmt(
        "<div class='xp-status'>"
        "<span class='sseg'>%d object%s</span>"
        "</div>", n, n==1?"":"s"
    );
    js_render(g_html);
}

/* ── DETAIL VIEW ─────────────────────────────────── */
static void render_detail(void) {
    char sql[256];
    snprintf(sql,sizeof sql,
        "SELECT m.id,m.media_type,m.title,m.release_year,"
        "m.poster_url,m.director,m.cast_list,m.description,"
        "e.status,e.rating,e.notes,e.favorite"
        " FROM media m JOIN user_media_experience e ON e.media_id=m.id"
        " WHERE m.id=%d;", g_detail_id);
    js_db_query(sql);
    if (!js_row_count()) { g_view=VIEW_LIST; render_list(); return; }

    char  *type   = js_get_str(0,"media_type");
    char  *title  = js_get_str(0,"title");
    int    year   = js_get_int(0,"release_year");
    char  *poster = js_get_str(0,"poster_url");
    char  *dir    = js_get_str(0,"director");
    char  *cast   = js_get_str(0,"cast_list");
    char  *desc   = js_get_str(0,"description");
    char  *status = js_get_str(0,"status");
    double rating = js_get_float(0,"rating");
    char  *notes  = js_get_str(0,"notes");
    int    fav    = js_get_int(0,"favorite");
    char   stars[64]=""; make_stars(stars,rating,5);

    int is_book_album = !strcmp(type,"book")||!strcmp(type,"album");

    h_reset(); render_toolbar();
    h_cat("<div class='xp-main'><div class='detail-layout'>");

    /* Poster */
    h_cat("<div class='detail-poster-col'>");
    if (poster&&*poster)
        h_fmt("<img class='detail-poster' src='%s' alt=''>",poster);
    else
        h_fmt("<div class='detail-poster-ph'>%s</div>",type_icon(type));
    h_cat("</div>");

    /* Info */
    h_fmt("<div class='detail-info'>"
          "<div class='detail-title'>%s%s</div>",
        title, fav?" <span class='fav-mark'>♥</span>":"");

    /* Properties */
    h_cat("<div class='xp-group'><div class='xp-group-title'>Properties</div>"
          "<table class='prop-table'>");
    h_fmt("<tr><td>Type</td><td><span class='type-badge type-%s'>%s %s</span></td></tr>",
        type,type_icon(type),type);
    if (year>0)
        h_fmt("<tr><td>Year</td><td>%d</td></tr>",year);
    h_fmt("<tr><td>Status</td><td><span class='status-badge s-%s'>%s</span></td></tr>",
        status,status_label(status));
    if (rating>=0.5)
        h_fmt("<tr><td>Rating</td><td class='stars'>%s <span class='dim'>(%.1f / 5)</span></td></tr>",
            stars,rating);
    if (dir&&*dir)
        h_fmt("<tr><td>%s</td><td>%s</td></tr>",
            is_book_album?"Author/Artist":"Director", dir);
    if (cast&&*cast&&!is_book_album)
        h_fmt("<tr><td>Cast</td><td>%s</td></tr>",cast);
    h_cat("</table></div>");

    /* Description */
    if (desc&&*desc) {
        h_cat("<div class='xp-group'><div class='xp-group-title'>Description</div>");
        h_fmt("<div class='group-body detail-notes'>%s</div>",desc);
        h_cat("</div>");
    }
    /* Notes */
    if (notes&&*notes) {
        h_cat("<div class='xp-group'><div class='xp-group-title'>My Notes</div>");
        h_fmt("<div class='group-body detail-notes'>%s</div>",notes);
        h_cat("</div>");
    }

    h_fmt(
        "<div class='detail-actions'>"
        "<button class='xp-btn' onclick=\"C.nav('list','all')\">← Back</button>"
        "<button class='xp-btn danger' onclick='C.del(%d)'>🗑 Delete</button>"
        "</div>",
        g_detail_id
    );
    h_cat("</div></div></div>");
    h_fmt(
        "<div class='xp-status'>"
        "<span class='sseg'>%s</span>"
        "<span class='sseg'>%s</span>"
        "</div>",
        title, status_label(status)
    );

    free(type);free(title);free(poster);free(dir);
    free(cast);free(desc);free(status);free(notes);
    js_render(g_html);
}

/* ── ADD VIEW ────────────────────────────────────── */
static void render_add(void) {
    int is_book_album = !strcmp(g_add_type,"book")||!strcmp(g_add_type,"album");

    h_reset(); render_toolbar();
    h_cat("<div class='xp-main'><div class='form-area'>");

    /* Type selector */
    h_cat("<div class='xp-group'><div class='xp-group-title'>1. Select Media Type</div>"
          "<div class='type-selector'>");
    struct { const char *id,*icon,*lbl; } types[]={
        {"movie","⬡","Movie"},{"book","◈","Book"},
        {"tv","◫","TV Show"},{"album","◎","Album"},{"game","◆","Game"}
    };
    for (int i=0;i<5;i++) {
        int a=!strcmp(g_add_type,types[i].id);
        h_fmt("<button class='type-btn%s' onclick=\"C.setType('%s')\">%s %s</button>",
            a?" active":"",types[i].id,types[i].icon,types[i].lbl);
    }
    h_cat("</div></div>");

    /* Details */
    h_cat("<div class='xp-group'><div class='xp-group-title'>2. Enter Details</div>");

    h_cat(
        "<div class='frow'>"
        "<span class='flbl req'>Title</span>"
        "<input class='xp-input' id='ft' type='text' "
        "placeholder='Start typing to search...' autocomplete='off'>"
        "</div>"
        "<div class='frow'>"
        "<span class='flbl'>Year</span>"
        "<input class='xp-input' id='fy' type='number' "
        "placeholder='e.g. 2024' min='1800' max='2100' style='max-width:110px'>"
        "</div>"
        "<div class='frow'>"
        "<span class='flbl'>Status</span>"
        "<select class='xp-select' id='fs' style='max-width:160px'>"
        "<option value='want'>Want to</option>"
        "<option value='watching'>In Progress</option>"
        "<option value='completed'>Completed</option>"
        "<option value='dropped'>Dropped</option>"
        "</select></div>"
        "<div class='frow'>"
        "<span class='flbl'>Rating</span>"
        "<input class='xp-input' id='fr' type='number' "
        "placeholder='0 – 5' min='0' max='5' step='0.5' style='max-width:80px'>"
        "</div>"
        "<div class='frow'>"
        "<span class='flbl'>Favorite</span>"
        "<label><input type='checkbox' id='ff'> Mark as favorite ♥</label>"
        "</div>"
    );

    /* Poster */
    h_cat(
        "<div class='frow poster-frow'>"
        "<span class='flbl'>Poster</span>"
        "<input type='hidden' id='fp' value=''>"
        "<img id='poster-preview' style='display:none;"
        "max-height:50px;border:1px solid #aca899;margin-right:6px'>"
        "<button type='button' onclick='openPosterModal()' class='xp-btn'>🖼 Choose Poster</button>"
        "<span id='poster-hint' style='font-size:10px;color:#666;margin-left:6px'>"
        "Search a title first</span>"
        "</div>"
    );

    /* Director / artist */
    h_fmt(
        "<div class='frow'>"
        "<span class='flbl'>%s</span>"
        "<input class='xp-input' id='fd' type='text' "
        "placeholder='Auto-filled from search'>"
        "</div>",
        is_book_album ? "Author/Artist" : "Director"
    );

    if (!is_book_album) {
        h_cat(
            "<div class='frow'>"
            "<span class='flbl'>Cast</span>"
            "<input class='xp-input' id='fc' type='text' "
            "placeholder='Auto-filled from search'>"
            "</div>"
        );
    } else {
        h_cat("<input type='hidden' id='fc' value=''>");
    }

    h_cat(
        "<div class='frow'>"
        "<span class='flbl'>Notes</span>"
        "<textarea class='xp-textarea' id='fn' "
        "placeholder='Your thoughts...'></textarea>"
        "</div>"
    );
    h_cat("</div>"); /* end group */

    h_cat(
        "<div class='form-actions'>"
        "<button onclick='C.submit()' class='xp-btn primary'>✓ Add Entry</button>"
        "<button onclick=\"C.nav('list','all')\" class='xp-btn'>✕ Cancel</button>"
        "</div>"
    );

    h_cat("</div></div>"); /* form-area + main */
    h_cat(
        "<div class='xp-status'>"
        "<span class='sseg'>Add new entry to library</span>"
        "</div>"
    );
    js_render(g_html);
}

/* ── STATS VIEW ──────────────────────────────────── */
static void render_stats(void) {
    js_db_query("SELECT COUNT(*) AS n FROM media;");
    int total=js_get_int(0,"n");
    js_db_query("SELECT COUNT(*) AS n FROM user_media_experience WHERE favorite=1;");
    int favs=js_get_int(0,"n");
    js_db_query("SELECT COUNT(*) AS n FROM user_media_experience WHERE status='completed';");
    int done=js_get_int(0,"n");
    js_db_query("SELECT COALESCE(AVG(rating),0) AS r FROM user_media_experience WHERE rating IS NOT NULL;");
    double avg=js_get_float(0,"r");

    js_db_query("SELECT media_type,COUNT(*) AS n FROM media GROUP BY media_type ORDER BY n DESC;");
    char tn[8][32]; int tv[8]; int nt=js_row_count(); if(nt>8)nt=8;
    for(int i=0;i<nt;i++){char*t=js_get_str(i,"media_type");strncpy(tn[i],t,31);tn[i][31]='\0';tv[i]=js_get_int(i,"n");free(t);}

    js_db_query("SELECT status,COUNT(*) AS n FROM user_media_experience GROUP BY status ORDER BY n DESC;");
    char sn[6][32]; int sv[6]; int ns=js_row_count(); if(ns>6)ns=6;
    for(int i=0;i<ns;i++){char*s=js_get_str(i,"status");strncpy(sn[i],s,31);sn[i][31]='\0';sv[i]=js_get_int(i,"n");free(s);}

    h_reset(); render_toolbar();
    h_cat("<div class='xp-main'>");

    h_cat("<div class='stats-grid'>");
    h_fmt(
        "<div class='stat-box'><div class='snum'>%d</div><div class='slbl'>Tracked</div></div>"
        "<div class='stat-box'><div class='snum'>%d</div><div class='slbl'>Completed</div></div>"
        "<div class='stat-box'><div class='snum'>%d</div><div class='slbl'>Favorites</div></div>"
        "<div class='stat-box'><div class='snum'>%.1f</div><div class='slbl'>Avg Rating</div></div>",
        total,done,favs,avg
    );
    h_cat("</div>");

    h_cat("<div class='xp-group'><div class='xp-group-title'>By Type</div><table class='bar-table'>");
    for(int i=0;i<nt;i++){
        int pct=total>0?(tv[i]*100)/total:0;
        h_fmt(
            "<tr><td class='bar-lbl'>%s %s</td>"
            "<td><div class='bar-track'><div class='bar-fill bf-%s' style='width:%d%%'></div></div></td>"
            "<td class='bar-cnt'>%d</td></tr>",
            type_icon(tn[i]),tn[i],tn[i],pct,tv[i]
        );
    }
    h_cat("</table></div>");

    h_cat("<div class='xp-group'><div class='xp-group-title'>By Status</div><table class='bar-table'>");
    for(int i=0;i<ns;i++){
        int pct=total>0?(sv[i]*100)/total:0;
        h_fmt(
            "<tr><td class='bar-lbl'>%s</td>"
            "<td><div class='bar-track'><div class='bar-fill bf-%s' style='width:%d%%'></div></div></td>"
            "<td class='bar-cnt'>%d</td></tr>",
            status_label(sn[i]),sn[i],pct,sv[i]
        );
    }
    h_cat("</table></div>");

    js_db_query(
        "SELECT m.title,m.media_type,e.rating"
        " FROM media m JOIN user_media_experience e ON e.media_id=m.id"
        " WHERE e.rating IS NOT NULL"
        " ORDER BY e.rating DESC, m.created_at DESC LIMIT 5;"
    );
    int nr=js_row_count();
    if(nr>0){
        h_cat("<div class='xp-group'><div class='xp-group-title'>Top Rated</div>"
              "<table class='xp-list'><tbody>");
        for(int i=0;i<nr;i++){
            char*t=js_get_str(i,"title"),*mt=js_get_str(i,"media_type");
            double r=js_get_float(i,"rating");
            char stars[64]=""; make_stars(stars,r,5);
            h_fmt("<tr><td style='width:30px;text-align:center'>%s</td>"
                  "<td>%s</td><td class='stars'>%s</td></tr>",
                type_icon(mt),t,stars);
            free(t);free(mt);
        }
        h_cat("</tbody></table></div>");
    }

    h_cat("</div>");
    h_fmt("<div class='xp-status'><span class='sseg'>%d items total</span></div>",total);
    js_render(g_html);
}

/* ── EXPORTED FUNCTIONS ──────────────────────────── */
EMSCRIPTEN_KEEPALIVE void app_init(void)  { js_init_db(); }
EMSCRIPTEN_KEEPALIVE void app_start(void) { render_list(); }

EMSCRIPTEN_KEEPALIVE void app_navigate(const char *view, const char *filter) {
    if (!strcmp(view,"list")) {
        g_view=VIEW_LIST;
        if(filter&&*filter){strncpy(g_filter,filter,sizeof g_filter-1);g_filter[sizeof g_filter-1]='\0';}
        g_search[0]='\0'; render_list();
    } else if (!strcmp(view,"add")) {
        g_view=VIEW_ADD; render_add();
    } else if (!strcmp(view,"stats")) {
        g_view=VIEW_STATS; render_stats();
    }
}

EMSCRIPTEN_KEEPALIVE void app_set_type(const char *type) {
    strncpy(g_add_type,type,sizeof g_add_type-1);
    g_add_type[sizeof g_add_type-1]='\0';
    render_add();
}

EMSCRIPTEN_KEEPALIVE void app_detail(int id) {
    g_view=VIEW_DETAIL; g_detail_id=id; render_detail();
}

EMSCRIPTEN_KEEPALIVE void app_delete(int id) {
    if(!js_confirm("Remove this item from your library?")) return;
    char sql[128]; snprintf(sql,sizeof sql,"DELETE FROM media WHERE id=%d;",id);
    js_db_exec(sql);
    g_view=VIEW_LIST; render_list();
}

EMSCRIPTEN_KEEPALIVE void app_search(const char *q) {
    strncpy(g_search,q?q:"",sizeof g_search-1);
    g_search[sizeof g_search-1]='\0';
    g_view=VIEW_LIST; render_list();
}

EMSCRIPTEN_KEEPALIVE
void app_submit(const char *title, const char *type, int year,
                const char *status, double rating, const char *notes,
                int favorite, const char *poster_url,
                const char *director, const char *cast_list) {
    if(!title||!*title) return;
    char st[1024],sn[4096],sp[4096],sd[1024],sc[2048];
    sql_esc(title,                  st, sizeof st);
    sql_esc(notes?notes:"",         sn, sizeof sn);
    sql_esc(poster_url?poster_url:"",sp, sizeof sp);
    sql_esc(director?director:"",   sd, sizeof sd);
    sql_esc(cast_list?cast_list:"", sc, sizeof sc);

    char sql[4096];
    if (year>0)
        snprintf(sql,sizeof sql,
            "INSERT INTO media(media_type,title,release_year,poster_url,director,cast_list)"
            " VALUES('%s','%s',%d,'%s','%s','%s');",
            type,st,year,sp,sd,sc);
    else
        snprintf(sql,sizeof sql,
            "INSERT INTO media(media_type,title,poster_url,director,cast_list)"
            " VALUES('%s','%s','%s','%s','%s');",
            type,st,sp,sd,sc);
    js_db_exec(sql);

    js_db_query("SELECT last_insert_rowid() AS id;");
    int mid=js_get_int(0,"id");

    if(rating>=0.5)
        snprintf(sql,sizeof sql,
            "INSERT INTO user_media_experience(media_id,status,rating,notes,favorite)"
            " VALUES(%d,'%s',%.1f,'%s',%d);",
            mid,status,rating,sn,favorite);
    else
        snprintf(sql,sizeof sql,
            "INSERT INTO user_media_experience(media_id,status,rating,notes,favorite)"
            " VALUES(%d,'%s',NULL,'%s',%d);",
            mid,status,sn,favorite);
    js_db_exec(sql);

    g_view=VIEW_LIST;
    strncpy(g_filter,"all",sizeof g_filter-1);
    g_search[0]='\0';
    render_list();
}

int main(void) { return 0; }