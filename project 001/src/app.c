/*
 * app.c — MediaLog: Personal Media Tracker
 * Entire frontend written in C, compiled to WebAssembly via Emscripten.
 *
 * Build:  see Makefile  (requires emcc + Emscripten SDK)
 * Run:    make serve    (python http.server on :8080)
 */

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ═══════════════════════════════════════════════════════════
   HTML BUFFER — C builds the entire UI as an HTML string,
   then pushes it to the DOM in one shot via js_render().
═══════════════════════════════════════════════════════════ */
#define HMAX (512 * 1024)   /* 512 KB ought to be enough */
static char   g_html[HMAX];
static size_t g_hlen = 0;

static void h_reset(void) {
    g_html[0] = '\0';
    g_hlen    = 0;
}

static void h_cat(const char *s) {
    size_t n = strlen(s);
    if (g_hlen + n < HMAX - 1) {
        memcpy(g_html + g_hlen, s, n + 1);
        g_hlen += n;
    }
}

static void h_fmt(const char *fmt, ...) {
    char    tmp[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    h_cat(tmp);
}

/* ═══════════════════════════════════════════════════════════
   APP STATE
═══════════════════════════════════════════════════════════ */
typedef enum { VIEW_LIST, VIEW_ADD, VIEW_DETAIL, VIEW_STATS } AppView;

static AppView g_view      = VIEW_LIST;
static char    g_filter[32] = "all";
static char    g_add_type[32] = "movie";
static int     g_detail_id   = 0;
static char    g_search[256]  = "";

/* ═══════════════════════════════════════════════════════════
   JS ↔ C BRIDGE
   EM_JS defines a JS function callable from C.
   These are the only places JavaScript appears in this project.
═══════════════════════════════════════════════════════════ */

/* Push the HTML buffer into #app */
EM_JS(void, js_render, (const char *ptr), {
    document.getElementById('app').innerHTML = UTF8ToString(ptr);
    /* Restore scroll position to top for new views */
    window.scrollTo(0, 0);
});

/* Execute SQL with no result set */
EM_JS(void, js_db_exec, (const char *ptr), {
    try {
        Module._db.run(UTF8ToString(ptr));
    } catch (e) {
        console.error('[db_exec]', e.message, '\nSQL:', UTF8ToString(ptr));
    }
});

/* Execute a SELECT; results stored in Module._rows */
EM_JS(void, js_db_query, (const char *ptr), {
    Module._rows = [];
    try {
        const stmt = Module._db.prepare(UTF8ToString(ptr));
        while (stmt.step()) Module._rows.push(stmt.getAsObject());
        stmt.free();
    } catch (e) {
        console.error('[db_query]', e.message, '\nSQL:', UTF8ToString(ptr));
    }
});

/* Number of rows from last query */
EM_JS(int, js_row_count, (), {
    return Module._rows ? Module._rows.length : 0;
});

/* Get a text cell — C caller must free() the returned pointer */
EM_JS(char *, js_get_str, (int row, const char *col_ptr), {
    const col = UTF8ToString(col_ptr);
    const obj = Module._rows[row];
    const val = (obj && col in obj && obj[col] !== null && obj[col] !== undefined)
                ? String(obj[col]) : '';
    const len = lengthBytesUTF8(val) + 1;
    const buf = _malloc(len);
    stringToUTF8(val, buf, len);
    return buf;
});

/* Get an integer cell */
EM_JS(int, js_get_int, (int row, const char *col_ptr), {
    const col = UTF8ToString(col_ptr);
    const obj = Module._rows[row];
    const v   = (obj && col in obj) ? obj[col] : null;
    return (v === null || v === undefined) ? 0 : (parseInt(v) | 0);
});

/* Get a float cell */
EM_JS(double, js_get_float, (int row, const char *col_ptr), {
    const col = UTF8ToString(col_ptr);
    const obj = Module._rows[row];
    const v   = (obj && col in obj) ? obj[col] : null;
    return (v === null || v === undefined) ? 0.0 : parseFloat(v);
});

/* Create schema + seed, then call app_start() */
EM_JS(void, js_init_db, (), {
    Module._db = new Module._SQL.Database();
    Module._db.run(`
        PRAGMA foreign_keys = ON;

        CREATE TABLE IF NOT EXISTS media (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            media_type   TEXT    NOT NULL,
            title        TEXT    NOT NULL,
            description  TEXT,
            release_year INTEGER,
            created_at   DATETIME DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS user_media_experience (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            media_id      INTEGER NOT NULL,
            status        TEXT    DEFAULT 'want',
            rating        REAL,
            notes         TEXT,
            rewatch_count INTEGER DEFAULT 0,
            favorite      INTEGER DEFAULT 0,
            created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(media_id) REFERENCES media(id) ON DELETE CASCADE
        );

        CREATE INDEX IF NOT EXISTS idx_media_type ON media(media_type);
        CREATE INDEX IF NOT EXISTS idx_media_title ON media(title);
    `);

    /* Seed a few demo items on first load */
    const count = Module._db.exec("SELECT COUNT(*) AS n FROM media;")[0].values[0][0];
    if (count === 0) {
        const seed = [
            ["movie","Dune: Part Two",2024,"completed",4.5,"Visually stunning sequel.",0],
            ["book","Project Hail Mary",2021,"completed",5,"Best sci-fi in years.",1],
            ["tv","Severance",2022,"watching",4,"Mind-bending workplace thriller.",0],
            ["album","Bright Future",2024,"completed",4.5,"Adrianne Lenker at her finest.",1],
            ["game","Hollow Knight",2017,"want",0,"On my backlog forever.",0],
        ];
        seed.forEach(([type,title,year,status,rating,notes,fav]) => {
            Module._db.run(
                "INSERT INTO media(media_type,title,release_year) VALUES(?,?,?);",
                [type, title, year]
            );
            const id = Module._db.exec("SELECT last_insert_rowid() AS id;")[0].values[0][0];
            Module._db.run(
                "INSERT INTO user_media_experience(media_id,status,rating,notes,favorite) VALUES(?,?,?,?,?);",
                [id, status, rating || null, notes, fav]
            );
        });
    }

    /* Hand control back to C */
    Module._app_start();
});

/* Show a browser confirm dialog, return 1 if OK */
EM_JS(int, js_confirm, (const char *ptr), {
    return window.confirm(UTF8ToString(ptr)) ? 1 : 0;
});

/* ═══════════════════════════════════════════════════════════
   UTILITY HELPERS
═══════════════════════════════════════════════════════════ */

static const char *type_icon(const char *t) {
    if (!t || !*t)            return "▣";
    if (!strcmp(t,"movie"))   return "⬡";   /* hexagon = movie reel */
    if (!strcmp(t,"book"))    return "◈";
    if (!strcmp(t,"tv"))      return "◫";
    if (!strcmp(t,"album"))   return "◎";
    if (!strcmp(t,"game"))    return "◆";
    return "▣";
}

static const char *status_label(const char *s) {
    if (!s || !*s)                  return "–";
    if (!strcmp(s,"completed"))     return "Completed";
    if (!strcmp(s,"watching"))      return "In Progress";
    if (!strcmp(s,"want"))          return "Want to";
    if (!strcmp(s,"dropped"))       return "Dropped";
    return s;
}

/* Safe SQL single-quote escaping */
static void sql_esc(const char *src, char *dst, size_t max) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < max; i++) {
        if (src[i] == '\'') dst[j++] = '\'';
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Build a ★☆ star string into buf (must be >= 20 bytes) */
static void make_stars(char *buf, double rating, int max) {
    buf[0] = '\0';
    int full = (int)(rating + 0.25); /* round to nearest */
    for (int i = 1; i <= max; i++) {
        strcat(buf, i <= full ? "★" : "☆");
    }
}

/* ═══════════════════════════════════════════════════════════
   NAV BAR
═══════════════════════════════════════════════════════════ */
static void render_nav(void) {
    h_cat(
        "<nav class='nav'>"
        "<div class='nav-brand'>"
        "<span class='brand-glyph'>◈</span>"
        "<span class='brand-text'>MEDIALOG</span>"
        "</div>"
        "<div class='nav-center'>"
    );

    struct { const char *id; const char *lbl; } tabs[] = {
        {"all","ALL"},{"movie","FILM"},{"book","BOOKS"},
        {"tv","TV"},{"album","MUSIC"},{"game","GAMES"}
    };
    for (int i = 0; i < 6; i++) {
        int active = (g_view == VIEW_LIST) && !strcmp(g_filter, tabs[i].id);
        h_fmt(
            "<button class='tab%s' onclick=\"C.nav('list','%s')\">%s</button>",
            active ? " tab-active" : "", tabs[i].id, tabs[i].lbl
        );
    }

    h_cat(
        "</div>"
        "<div class='nav-right'>"
        "<button class='nav-btn' onclick=\"C.nav('stats','')\""
    );
    if (g_view == VIEW_STATS) h_cat(" style='opacity:1;border-color:var(--accent)'");
    h_cat(
        ">STATS</button>"
        "<button class='nav-btn nav-add' onclick=\"C.nav('add','')\">＋ ADD</button>"
        "</div>"
        "</nav>"
    );
}

/* ═══════════════════════════════════════════════════════════
   LIST VIEW
═══════════════════════════════════════════════════════════ */
static void render_list(void) {
    char sql[1024];
    const char *where = strcmp(g_filter,"all")==0
        ? "" : " AND m.media_type=?type";  /* won't use this, see below */

    /* Build query — filter by type and optional search */
    char type_clause[64] = "";
    if (strcmp(g_filter,"all") != 0) {
        snprintf(type_clause, sizeof type_clause,
                 " AND m.media_type='%s'", g_filter);
    }
    char search_clause[512] = "";
    if (g_search[0]) {
        char esc[512];
        sql_esc(g_search, esc, sizeof esc);
        snprintf(search_clause, sizeof search_clause,
                 " AND LOWER(m.title) LIKE LOWER('%%%s%%')", esc);
    }

    snprintf(sql, sizeof sql,
        "SELECT m.id, m.media_type, m.title, m.release_year,"
        " e.status, e.rating, e.favorite, e.rewatch_count"
        " FROM media m"
        " LEFT JOIN user_media_experience e ON e.media_id = m.id"
        " WHERE 1=1%s%s"
        " ORDER BY e.favorite DESC, m.created_at DESC;",
        type_clause, search_clause);

    js_db_query(sql);
    int n = js_row_count();

    h_reset();
    render_nav();

    /* Search bar */
    h_fmt(
        "<div class='search-bar'>"
        "<input class='search-input' type='text' placeholder='Search titles...'"
        " value='%s' oninput='C.search(this.value)' id='srch'>"
        "</div>",
        g_search
    );

    h_cat("<main class='main'>");

    if (n == 0) {
        h_cat(
            "<div class='empty'>"
            "<div class='empty-glyph'>◌</div>"
            "<div class='empty-msg'>Nothing here yet</div>"
            "<div class='empty-sub'>Track something to get started</div>"
            "</div>"
        );
    } else {
        /* Stats strip */
        h_fmt("<div class='list-meta'>%d item%s</div>", n, n==1?"":"s");

        h_cat("<div class='grid'>");
        for (int i = 0; i < n; i++) {
            char *ids   = js_get_str(i, "id");
            char *type  = js_get_str(i, "media_type");
            char *title = js_get_str(i, "title");
            char *year  = js_get_str(i, "release_year");
            char *stat  = js_get_str(i, "status");
            double rat  = js_get_float(i, "rating");
            int fav     = js_get_int(i, "favorite");

            char stars[64] = "";
            make_stars(stars, rat, 5);

            h_fmt("<div class='card type-card-%s' onclick='C.detail(%s)'>", type, ids);

            /* Type glyph */
            h_fmt("<div class='card-glyph'>%s</div>", type_icon(type));

            h_cat("<div class='card-body'>");
            h_fmt("<div class='card-title'>%s</div>", title);

            h_cat("<div class='card-row'>");
            if (year && *year) h_fmt("<span class='card-year'>%s</span>", year);
            if (stat && *stat) {
                h_fmt("<span class='badge badge-%s'>%s</span>",
                      stat, status_label(stat));
            }
            if (fav) h_cat("<span class='fav-mark'>♥</span>");
            h_cat("</div>");

            if (rat > 0)
                h_fmt("<div class='card-stars'>%s</div>", stars);

            h_cat("</div>"); /* card-body */

            h_fmt(
                "<button class='card-del' title='Remove'"
                " onclick='event.stopPropagation();C.del(%s)'>✕</button>",
                ids
            );

            h_cat("</div>"); /* card */

            free(ids); free(type); free(title); free(year); free(stat);
        }
        h_cat("</div>"); /* grid */
    }

    h_cat("</main>");
    js_render(g_html);

    /* Refocus search box if there's a query */
    if (g_search[0]) {
        EM_ASM({
            const el = document.getElementById('srch');
            if (el) { el.focus(); el.setSelectionRange(9999,9999); }
        });
    }
}

/* ═══════════════════════════════════════════════════════════
   ADD VIEW
═══════════════════════════════════════════════════════════ */
static void render_add(void) {
    h_reset();
    render_nav();
    h_cat("<main class='main'><div class='form-wrap'>");
    h_cat("<div class='form-head'>ADD TO LIBRARY</div>");

    /* Type row */
    h_cat("<div class='form-section'>");
    h_cat("<div class='form-label'>TYPE</div>");
    h_cat("<div class='type-row'>");
    struct { const char *id; const char *ico; const char *lbl; } types[] = {
        {"movie","⬡","FILM"}, {"book","◈","BOOK"}, {"tv","◫","TV"},
        {"album","◎","MUSIC"},{"game","◆","GAME"}
    };
    for (int i = 0; i < 5; i++) {
        int act = !strcmp(g_add_type, types[i].id);
        h_fmt(
            "<button class='type-btn%s' onclick=\"C.setType('%s')\">"
            "<span class='type-btn-ico'>%s</span>"
            "<span>%s</span>"
            "</button>",
            act ? " type-btn-active" : "", types[i].id,
            types[i].ico, types[i].lbl
        );
    }
    h_cat("</div></div>");

    /* Title */
    h_cat(
        "<div class='form-section'>"
        "<label class='form-label' for='ft'>TITLE *</label>"
        "<input id='ft' class='f-in' type='text' placeholder='Enter title...' autofocus>"
        "</div>"
    );

    /* Year + Status side by side */
    h_cat("<div class='form-row'>");
    h_cat(
        "<div class='form-section'>"
        "<label class='form-label' for='fy'>YEAR</label>"
        "<input id='fy' class='f-in' type='number' placeholder='e.g. 2024' min='1800' max='2030'>"
        "</div>"
    );
    h_cat(
        "<div class='form-section'>"
        "<label class='form-label' for='fs'>STATUS</label>"
        "<select id='fs' class='f-in'>"
        "<option value='want'>☆  Want to</option>"
        "<option value='watching'>▶  In Progress</option>"
        "<option value='completed'>✓  Completed</option>"
        "<option value='dropped'>✗  Dropped</option>"
        "</select>"
        "</div>"
    );
    h_cat("</div>"); /* form-row */

    /* Rating + Favorite side by side */
    h_cat("<div class='form-row'>");
    h_cat(
        "<div class='form-section'>"
        "<label class='form-label' for='fr'>RATING (1–5)</label>"
        "<input id='fr' class='f-in' type='number' min='0.5' max='5' step='0.5' placeholder='Optional'>"
        "</div>"
    );
    h_cat(
        "<div class='form-section form-section-fav'>"
        "<div class='form-label'>FAVORITE</div>"
        "<label class='toggle'>"
        "<input id='ff' type='checkbox'>"
        "<span class='toggle-slider'></span>"
        "</label>"
        "</div>"
    );
    h_cat("</div>"); /* form-row */

    /* Notes */
    h_cat(
        "<div class='form-section'>"
        "<label class='form-label' for='fn'>NOTES</label>"
        "<textarea id='fn' class='f-in f-area' placeholder='Thoughts, quotes, references...'></textarea>"
        "</div>"
    );

    /* Actions */
    h_cat(
        "<div class='form-actions'>"
        "<button class='btn-ghost' onclick=\"C.nav('list','all')\">CANCEL</button>"
        "<button class='btn-submit' onclick='C.submit()'>ADD TO LIBRARY</button>"
        "</div>"
    );

    h_cat("</div></main>");
    js_render(g_html);
}

/* ═══════════════════════════════════════════════════════════
   DETAIL VIEW
═══════════════════════════════════════════════════════════ */
static void render_detail(void) {
    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT m.id, m.media_type, m.title, m.description, m.release_year,"
        " e.status, e.rating, e.notes, e.favorite, e.rewatch_count"
        " FROM media m"
        " LEFT JOIN user_media_experience e ON e.media_id = m.id"
        " WHERE m.id = %d;",
        g_detail_id);
    js_db_query(sql);

    if (js_row_count() == 0) {
        g_view = VIEW_LIST; render_list(); return;
    }

    char *type  = js_get_str(0, "media_type");
    char *title = js_get_str(0, "title");
    char *desc  = js_get_str(0, "description");
    char *year  = js_get_str(0, "release_year");
    char *stat  = js_get_str(0, "status");
    double rat  = js_get_float(0, "rating");
    char *notes = js_get_str(0, "notes");
    int fav     = js_get_int(0, "favorite");
    int rewat   = js_get_int(0, "rewatch_count");

    char stars[64] = "";
    make_stars(stars, rat, 5);

    h_reset();
    render_nav();

    h_fmt("<main class='main'><div class='detail-wrap type-detail-%s'>", type);

    /* Header */
    h_cat("<div class='detail-header'>");
    h_fmt("<div class='detail-glyph'>%s</div>", type_icon(type));
    h_cat("<div class='detail-head-text'>");
    h_fmt("<div class='detail-type-label'>%s</div>", type);
    h_fmt("<h1 class='detail-title'>%s</h1>", title);
    if (year && *year)
        h_fmt("<div class='detail-year'>%s</div>", year);
    h_cat("</div></div>"); /* detail-head-text, detail-header */

    /* Pills */
    h_cat("<div class='detail-pills'>");
    if (stat && *stat)
        h_fmt("<div class='pill badge-%s'>%s</div>", stat, status_label(stat));
    if (rat > 0.0)
        h_fmt("<div class='pill pill-stars'>%s</div>", stars);
    if (fav)
        h_cat("<div class='pill pill-fav'>♥ FAVORITE</div>");
    if (rewat > 0)
        h_fmt("<div class='pill'>↺ %d×</div>", rewat);
    h_cat("</div>");

    /* Notes / Description */
    if (notes && *notes) {
        h_fmt(
            "<div class='detail-block'>"
            "<div class='block-label'>NOTES</div>"
            "<div class='block-text'>%s</div>"
            "</div>",
            notes
        );
    }
    if (desc && *desc) {
        h_fmt(
            "<div class='detail-block'>"
            "<div class='block-label'>DESCRIPTION</div>"
            "<div class='block-text'>%s</div>"
            "</div>",
            desc
        );
    }

    /* Actions */
    h_fmt(
        "<div class='detail-actions'>"
        "<button class='btn-ghost' onclick=\"C.nav('list','all')\">← BACK</button>"
        "<button class='btn-danger' onclick='C.del(%d)'>DELETE</button>"
        "</div>",
        g_detail_id
    );

    h_cat("</div></main>");
    js_render(g_html);

    free(type); free(title); free(desc);
    free(year); free(stat); free(notes);
}

/* ═══════════════════════════════════════════════════════════
   STATS VIEW
═══════════════════════════════════════════════════════════ */
static void render_stats(void) {
    /* Count by type */
    js_db_query(
        "SELECT media_type, COUNT(*) AS n"
        " FROM media GROUP BY media_type ORDER BY n DESC;"
    );
    int nt = js_row_count();

    /* Count by status */
    js_db_query(
        "SELECT COALESCE(e.status,'want') AS status, COUNT(*) AS n"
        " FROM media m"
        " LEFT JOIN user_media_experience e ON e.media_id=m.id"
        " GROUP BY e.status ORDER BY n DESC;"
    );
    /* store status counts in local arrays */
    char stat_names[8][32];
    int  stat_vals[8];
    int  ns = js_row_count();
    if (ns > 8) ns = 8;
    for (int i = 0; i < ns; i++) {
        char *s = js_get_str(i, "status");
        strncpy(stat_names[i], s, 31);
        stat_names[i][31] = '\0';
        stat_vals[i] = js_get_int(i, "n");
        free(s);
    }

    /* Total, avg rating, favorites */
    js_db_query("SELECT COUNT(*) AS total FROM media;");
    int total = js_get_int(0, "total");

    js_db_query(
        "SELECT ROUND(AVG(rating),1) AS avg_r"
        " FROM user_media_experience WHERE rating IS NOT NULL;"
    );
    double avg_r = js_get_float(0, "avg_r");

    js_db_query(
        "SELECT COUNT(*) AS favs"
        " FROM user_media_experience WHERE favorite=1;"
    );
    int favs = js_get_int(0, "favs");

    js_db_query(
        "SELECT COUNT(*) AS done"
        " FROM user_media_experience WHERE status='completed';"
    );
    int done = js_get_int(0, "done");

    /* Re-query type counts for display (we overwrote _rows) */
    js_db_query(
        "SELECT media_type, COUNT(*) AS n"
        " FROM media GROUP BY media_type ORDER BY n DESC;"
    );
    /* store in local array */
    char type_names[8][32];
    int  type_vals[8];
    int  ntype = js_row_count();
    if (ntype > 8) ntype = 8;
    for (int i = 0; i < ntype; i++) {
        char *t = js_get_str(i, "media_type");
        strncpy(type_names[i], t, 31);
        type_names[i][31] = '\0';
        type_vals[i] = js_get_int(i, "n");
        free(t);
    }

    h_reset();
    render_nav();
    h_cat("<main class='main'><div class='stats-wrap'>");
    h_cat("<div class='stats-head'>LIBRARY STATS</div>");

    /* Big numbers */
    h_cat("<div class='stat-cards'>");
    h_fmt(
        "<div class='stat-card'><div class='stat-num'>%d</div><div class='stat-lbl'>TRACKED</div></div>"
        "<div class='stat-card'><div class='stat-num'>%d</div><div class='stat-lbl'>COMPLETED</div></div>"
        "<div class='stat-card'><div class='stat-num'>%d</div><div class='stat-lbl'>FAVORITES</div></div>"
        "<div class='stat-card'><div class='stat-num'>%.1f</div><div class='stat-lbl'>AVG RATING</div></div>",
        total, done, favs, avg_r
    );
    h_cat("</div>");

    /* By type */
    h_cat("<div class='stat-section'><div class='stat-section-title'>BY TYPE</div>");
    h_cat("<div class='bar-list'>");
    for (int i = 0; i < ntype; i++) {
        int pct = total > 0 ? (type_vals[i] * 100) / total : 0;
        h_fmt(
            "<div class='bar-row'>"
            "<div class='bar-label type-label-%s'>%s %s</div>"
            "<div class='bar-track'>"
            "<div class='bar-fill type-fill-%s' style='width:%d%%'></div>"
            "</div>"
            "<div class='bar-count'>%d</div>"
            "</div>",
            type_names[i], type_icon(type_names[i]), type_names[i],
            type_names[i], pct, type_vals[i]
        );
    }
    h_cat("</div></div>");

    /* By status */
    h_cat("<div class='stat-section'><div class='stat-section-title'>BY STATUS</div>");
    h_cat("<div class='bar-list'>");
    for (int i = 0; i < ns; i++) {
        int pct = total > 0 ? (stat_vals[i] * 100) / total : 0;
        h_fmt(
            "<div class='bar-row'>"
            "<div class='bar-label'>%s</div>"
            "<div class='bar-track'>"
            "<div class='bar-fill badge-%s' style='width:%d%%'></div>"
            "</div>"
            "<div class='bar-count'>%d</div>"
            "</div>",
            status_label(stat_names[i]),
            stat_names[i], pct, stat_vals[i]
        );
    }
    h_cat("</div></div>");

    /* Top rated */
    js_db_query(
        "SELECT m.title, m.media_type, e.rating"
        " FROM media m"
        " JOIN user_media_experience e ON e.media_id=m.id"
        " WHERE e.rating IS NOT NULL"
        " ORDER BY e.rating DESC, m.created_at DESC"
        " LIMIT 5;"
    );
    int nr = js_row_count();
    if (nr > 0) {
        h_cat("<div class='stat-section'><div class='stat-section-title'>TOP RATED</div>");
        h_cat("<div class='top-list'>");
        for (int i = 0; i < nr; i++) {
            char *t   = js_get_str(i, "title");
            char *mt  = js_get_str(i, "media_type");
            double r  = js_get_float(i, "rating");
            char stars[64] = "";
            make_stars(stars, r, 5);
            h_fmt(
                "<div class='top-item'>"
                "<span class='top-glyph'>%s</span>"
                "<span class='top-title'>%s</span>"
                "<span class='top-stars'>%s</span>"
                "</div>",
                type_icon(mt), t, stars
            );
            free(t); free(mt);
        }
        h_cat("</div></div>");
    }

    h_cat("</div></main>");
    js_render(g_html);
    (void)nt;
}

/* ═══════════════════════════════════════════════════════════
   EXPORTED FUNCTIONS  (called from JavaScript via C.*)
═══════════════════════════════════════════════════════════ */

EMSCRIPTEN_KEEPALIVE
void app_init(void) {
    js_init_db();
}

EMSCRIPTEN_KEEPALIVE
void app_start(void) {
    render_list();
}

EMSCRIPTEN_KEEPALIVE
void app_navigate(const char *view, const char *filter) {
    if (!strcmp(view, "list")) {
        g_view = VIEW_LIST;
        if (filter && *filter) {
            strncpy(g_filter, filter, sizeof g_filter - 1);
            g_filter[sizeof g_filter - 1] = '\0';
        }
        g_search[0] = '\0';
        render_list();
    } else if (!strcmp(view, "add")) {
        g_view = VIEW_ADD;
        render_add();
    } else if (!strcmp(view, "stats")) {
        g_view = VIEW_STATS;
        render_stats();
    }
}

EMSCRIPTEN_KEEPALIVE
void app_set_type(const char *type) {
    strncpy(g_add_type, type, sizeof g_add_type - 1);
    g_add_type[sizeof g_add_type - 1] = '\0';
    render_add();
}

EMSCRIPTEN_KEEPALIVE
void app_detail(int id) {
    g_view      = VIEW_DETAIL;
    g_detail_id = id;
    render_detail();
}

EMSCRIPTEN_KEEPALIVE
void app_delete(int id) {
    if (!js_confirm("Remove this item from your library?")) return;
    char sql[128];
    snprintf(sql, sizeof sql, "DELETE FROM media WHERE id=%d;", id);
    js_db_exec(sql);
    g_view = VIEW_LIST;
    render_list();
}

EMSCRIPTEN_KEEPALIVE
void app_search(const char *q) {
    strncpy(g_search, q ? q : "", sizeof g_search - 1);
    g_search[sizeof g_search - 1] = '\0';
    g_view = VIEW_LIST;
    render_list();
}

EMSCRIPTEN_KEEPALIVE
void app_submit(const char *title, const char *type, int year,
                const char *status, double rating,
                const char *notes, int favorite) {
    if (!title || !*title) return;

    char safe_t[1024], safe_n[4096];
    sql_esc(title,           safe_t, sizeof safe_t);
    sql_esc(notes ? notes : "", safe_n, sizeof safe_n);

    char sql[2048];
    if (year > 0) {
        snprintf(sql, sizeof sql,
            "INSERT INTO media(media_type,title,release_year)"
            " VALUES('%s','%s',%d);",
            type, safe_t, year);
    } else {
        snprintf(sql, sizeof sql,
            "INSERT INTO media(media_type,title)"
            " VALUES('%s','%s');",
            type, safe_t);
    }
    js_db_exec(sql);

    js_db_query("SELECT last_insert_rowid() AS id;");
    int media_id = js_get_int(0, "id");

    if (rating >= 0.5) {
        snprintf(sql, sizeof sql,
            "INSERT INTO user_media_experience"
            "(media_id,status,rating,notes,favorite)"
            " VALUES(%d,'%s',%.1f,'%s',%d);",
            media_id, status, rating, safe_n, favorite);
    } else {
        snprintf(sql, sizeof sql,
            "INSERT INTO user_media_experience"
            "(media_id,status,rating,notes,favorite)"
            " VALUES(%d,'%s',NULL,'%s',%d);",
            media_id, status, safe_n, favorite);
    }
    js_db_exec(sql);

    g_view = VIEW_LIST;
    strncpy(g_filter, "all", sizeof g_filter - 1);
    g_search[0] = '\0';
    render_list();
}

/* ═══════════════════════════════════════════════════════════
   MAIN  — Emscripten calls this; real init is app_init()
   which is called from JS after sql.js is ready.
═══════════════════════════════════════════════════════════ */
int main(void) {
    return 0;
}
