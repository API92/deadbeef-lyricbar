#include "utils.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype> // ::isspace
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

#include <giomm.h>
#include <glibmm/fileutils.h>
#include <glibmm/uriutils.h>
#include <libsoup/soup.h>

#include "debug.h"
#include "gettext.h"
#include "ui.h"

using namespace std;
using namespace Glib;

const DB_playItem_t *last;

static const ustring AZL_FMT = "https://www.azlyrics.com/lyrics/%1/%2.html";
static const char *home_cache = getenv("XDG_CACHE_HOME");
static const string lyrics_dir = (home_cache ? string(home_cache) : string(getenv("HOME")) + "/.cache")
                               + "/deadbeef/lyrics/";

static experimental::optional<ustring>(*const providers[])(DB_playItem_t *) = {&get_lyrics_from_script, &download_lyrics_from_azlyrics};

inline string cached_filename(string artist, string title) {
	replace(artist.begin(), artist.end(), '/', '_');
	replace(title.begin(), title.end(), '/', '_');

	return lyrics_dir + artist + '-' + title;
}

extern "C"
bool is_cached(const char *artist, const char *title) {
	return artist && title && access(cached_filename(artist, title).c_str(), 0) == 0;
}

extern "C"
void ensure_lyrics_path_exists() {
	mkpath(lyrics_dir, 0755);
}

/**
 * Loads the cached lyrics
 * @param artist The artist name
 * @param title  The song title
 * @note         Have no idea about the encodings, so a bug possible here
 */
experimental::optional<ustring> load_cached_lyrics(const char *artist, const char *title) {
	string filename = cached_filename(artist, title);
	debug_out << "filename = '" << filename << "'\n";
	try {
		return {file_get_contents(filename)};
	} catch (const FileError& error) {
		debug_out << error.what();
		return {};
	}
}

bool save_cached_lyrics(const string &artist, const string &title, const string &lyrics) {
	string filename = cached_filename(artist, title);
	ofstream t(filename);
	if (!t) {
		cerr << "lyricbar: could not open file for writing: " << filename << endl;
		return false;
	}
	t << lyrics;
	return true;
}

bool is_playing(DB_playItem_t *track) {
	DB_playItem_t *pl_track = deadbeef->streamer_get_playing_track();
	if (!pl_track)
		return false;

	deadbeef->pl_item_unref(pl_track);
	return pl_track == track;
}

static
experimental::optional<ustring> get_lyrics_from_metadata(DB_playItem_t *track) {
	pl_lock_guard guard;
	const char *lyrics = deadbeef->pl_find_meta(track, "unsynced lyrics")
	                  ?: deadbeef->pl_find_meta(track, "UNSYNCEDLYRICS")
	                  ?: deadbeef->pl_find_meta(track, "lyrics");
	if (lyrics)
		return ustring{lyrics};
	else return {};
}

experimental::optional<ustring> get_lyrics_from_script(DB_playItem_t *track) {
	std::string buf = std::string(4096, '\0');
	deadbeef->conf_get_str("lyricbar.customcmd", nullptr, &buf[0], buf.size());
	if (!buf[0]) {
		return {};
	}
	auto tf_code = deadbeef->tf_compile(buf.data());
	if (!tf_code) {
		std::cerr << "lyricbar: Invalid script command!\n";
		return {};
	}
	ddb_tf_context_t ctx{};
	ctx._size = sizeof(ctx);
	ctx.it = track;

	int command_len = deadbeef->tf_eval(&ctx, tf_code, &buf[0], buf.size());
	deadbeef->tf_free(tf_code);
	if (command_len < 0) {
		std::cerr << "lyricbar: Invalid script command!\n";
		return {};
	}

	buf.resize(command_len);

	std::string script_output;
	int exit_status = 0;
	try {
		spawn_command_line_sync(buf, &script_output, nullptr, &exit_status);
	} catch (const Glib::Error &e) {
		std::cerr << "lyricbar: " << e.what() << "\n";
		return {};
	}

	if (script_output.empty() || exit_status != 0) {
		return {};
	}

	auto res = ustring{std::move(script_output)};
	if (!res.validate()) {
		cerr << "lyricbar: script output is not a valid UTF8 string!\n";
		return {};
	}
	return {std::move(res)};
}

void alphadigitize(ustring &s) {
    ustring res;
    for (auto c : s)
        if (g_unichar_isalpha(c) || g_unichar_isdigit(c))
            res.push_back(c);
    s = res;
}

template<typename T>
using g_unq_ptr = std::unique_ptr<T, void (*)(gpointer)>;

experimental::optional<std::string> fetch_file_as_chrome(const std::string &uri) {
    g_unq_ptr<SoupSession> ses(soup_session_new_with_options("timeout", 10, nullptr), g_object_unref);
    g_unq_ptr<SoupMessage> msg(soup_message_new("GET", uri.c_str()), g_object_unref);
    soup_message_headers_append(msg->request_headers, "User-Agent",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.114 Safari/537.36");
    g_unq_ptr<GInputStream> stream(soup_session_send(ses.get(), msg.get(), nullptr, nullptr), g_object_unref);
    if (!stream || msg->status_code / 100 != 2)
        return {};

    std::array<char, 4096> buf;
    std::string res;
    constexpr size_t MAX_FILE_SIZE = size_t{1} << 20U; // 1MB outta be enough
    while (true) {
        auto nbytes = g_input_stream_read(stream.get(), buf.data(), buf.size(), nullptr, nullptr);
        if (nbytes > 0) {
            if (res.size() + nbytes > MAX_FILE_SIZE) {
                cerr << "lyricbar: file '" << uri << "' too large!\n";
                throw std::runtime_error("file too large");
            }
            res.append(buf.data(), nbytes);
        } else {
            assert(nbytes == 0);
            return res;
        }
    }

    return {};
}

experimental::optional<ustring> download_lyrics_from_azlyrics(DB_playItem_t *track) {
    ustring artist;
    ustring title;
    {
        pl_lock_guard guard;
        const char *artist_raw, *title_raw;
        artist_raw = deadbeef->pl_find_meta(track, "artist");
        title_raw  = deadbeef->pl_find_meta(track, "title");
        if (!artist_raw || !title_raw) {
            return {};
        }
        artist = artist_raw;
        title = title_raw;
    }
    artist = artist.lowercase();
    title = title.lowercase();
    alphadigitize(artist);

    experimental::optional<std::string> doc;
    for (;;) {
        ustring norm_title = title;
        alphadigitize(norm_title);
        ustring api_url = ustring::compose(AZL_FMT,
                uri_escape_string(artist, {}, false),
                uri_escape_string(norm_title, {}, false));

        doc = fetch_file_as_chrome(api_url);
        if (doc)
            break;

        while (!title.empty() &&
                *title.rbegin() != '(' && *title.rbegin() != ')' &&
                *title.rbegin() != '[' && *title.rbegin() != ']' )
            title.resize(title.size() - 1);

        if (title.empty())
            return {};
        title.resize(title.size() - 1);
    }
    if (!doc)
        return {};

    const static regex r{R"(<div>\s*<!--\s*Usage of azlyrics.com content[^]*?-->\s*([^]*?)\s*</div>)"};
    smatch match;
    regex_search(*doc, match, r);
    if (match.size() < 2) {
        return {};
    }

    std::string lyrics = match[1];
    static const regex br_expr(R"(<br\s*/?\s*>)");
    lyrics = regex_replace(lyrics, br_expr, "");
    static const regex tag_expr(R"(<[^>]*>)");
    lyrics = regex_replace(lyrics, tag_expr, "");
    static const regex quot_expr(R"(&quot;)");
    lyrics = regex_replace(lyrics, quot_expr, "\"");
    if (lyrics.empty())
        return {};
    if (lyrics.back() != '\n')
        lyrics.push_back('\n');
    return ustring(lyrics);
}

void update_lyrics(void *tr) {
	DB_playItem_t *track = static_cast<DB_playItem_t*>(tr);

	if (auto lyrics = get_lyrics_from_metadata(track)) {
		set_lyrics(track, *lyrics);
		return;
	}

	const char *artist;
	const char *title;
	{
		pl_lock_guard guard;
		artist = deadbeef->pl_find_meta(track, "artist");
		title  = deadbeef->pl_find_meta(track, "title");
	}

	if (artist && title) {
		if (auto lyrics = load_cached_lyrics(artist, title)) {
			set_lyrics(track, *lyrics);
			return;
		}

		set_lyrics(track, _("Loading..."));

		// No lyrics in the tag or cache; try to get some and cache if succeeded
		for (auto f : providers) {
			if (auto lyrics = f(track)) {
				set_lyrics(track, *lyrics);
				save_cached_lyrics(artist, title, *lyrics);
				return;
			}
		}
	}
	set_lyrics(track, _("Lyrics not found"));
}

/**
 * Creates the directory tree.
 * @param name the directory path, including trailing slash
 * @return 0 on success; errno after mkdir call if something went wrong
 */
int mkpath(const string &name, mode_t mode) {
	string dir;
	size_t pos = 0;
	while ((pos = name.find_first_of('/', pos)) != string::npos){
		dir = name.substr(0, pos++);
		if (dir.empty())
			continue; // ignore the leading slash
		if (mkdir(dir.c_str(), mode) && errno != EEXIST)
			return errno;
	}
	return 0;
}

int remove_from_cache_action(DB_plugin_action_t *, int ctx) {
	if (ctx == DDB_ACTION_CTX_SELECTION) {
		pl_lock_guard guard;

		ddb_playlist_t *playlist = deadbeef->plt_get_curr();
		if (playlist) {
			DB_playItem_t *current = deadbeef->plt_get_first(playlist, PL_MAIN);
			while (current) {
				if (deadbeef->pl_is_selected (current)) {
					const char *artist = deadbeef->pl_find_meta(current, "artist");
					const char *title  = deadbeef->pl_find_meta(current, "title");
					if (is_cached(artist, title))
						remove(cached_filename(artist, title).c_str());
				}
				DB_playItem_t *next = deadbeef->pl_get_next(current, PL_MAIN);
				deadbeef->pl_item_unref(current);
				current = next;
			}
			deadbeef->plt_unref(playlist);
		}
	}
	return 0;
}
