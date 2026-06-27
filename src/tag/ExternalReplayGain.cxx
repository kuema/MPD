#include "config.h"
#include "ExternalReplayGain.hxx"

#include "Log.hxx"
#include "config/Data.hxx"
#include "config/Option.hxx"
#include "fs/AllocatedPath.hxx"
#include <string_view>
#include "util/Domain.hxx"

#include <sqlite3.h>

#include <string>

static constexpr Domain external_replay_gain_domain("external_replaygain");

static std::string replay_gain_external_db_path;

static constexpr char replay_gain_schema_sql[] =
	"CREATE TABLE IF NOT EXISTS replaygain ("
	"uri TEXT PRIMARY KEY,"
	"size INTEGER,"
	"mtime INTEGER,"
	"track_gain REAL,"
	"track_peak REAL,"
	"album_gain REAL,"
	"album_peak REAL,"
	"scanner TEXT,"
	"scanner_version TEXT,"
	"scanned_at INTEGER NOT NULL DEFAULT (unixepoch()),"
	"target_lufs REAL,"
	"track_loudness_lufs REAL,"
	"track_peak_db REAL,"
	"track_peak_type TEXT,"
	"track_clipping_adjustment INTEGER,"
	"album_loudness_lufs REAL,"
	"album_peak_db REAL,"
	"album_peak_type TEXT,"
	"album_clipping_adjustment INTEGER"
	")";

struct SqliteDb {
	sqlite3 *db = nullptr;

	~SqliteDb() noexcept {
		if (db != nullptr)
			sqlite3_close(db);
	}
};

struct SqliteStmt {
	sqlite3_stmt *stmt = nullptr;

	~SqliteStmt() noexcept {
		if (stmt != nullptr)
			sqlite3_finalize(stmt);
	}
};

static const char *
SqliteError(sqlite3 *db) noexcept
{
	return db != nullptr
		? sqlite3_errmsg(db)
		: "unknown SQLite error";
}

static bool
InitializeDatabase(const char *path) noexcept
{
	SqliteDb db;

	if (sqlite3_open_v2(path, &db.db,
			    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
			    nullptr) != SQLITE_OK) {
		FmtWarning(external_replay_gain_domain,
			   "external ReplayGain database init failed: {}: {}",
			   path, SqliteError(db.db));
		return false;
	}

	char *error = nullptr;
	if (sqlite3_exec(db.db, replay_gain_schema_sql, nullptr, nullptr, &error) != SQLITE_OK) {
		FmtWarning(external_replay_gain_domain,
			   "external ReplayGain schema init failed: {}: {}",
			   path, error != nullptr ? error : SqliteError(db.db));

		sqlite3_free(error);
		return false;
	}

	return true;
}

void
replay_gain_external_init(const ConfigData &config)
{
	const auto path = config.GetPath(ConfigOption::REPLAYGAIN_EXTERNAL_DB);

	if (path.IsNull()) {
		replay_gain_external_db_path.clear();
		LogNotice(external_replay_gain_domain,
			  "external ReplayGain database disabled");
		return;
	}

	replay_gain_external_db_path = path.c_str();

	if (InitializeDatabase(replay_gain_external_db_path.c_str()))
		FmtNotice(external_replay_gain_domain,
			  "external ReplayGain database configured: {}",
			  replay_gain_external_db_path);
	else
		FmtWarning(external_replay_gain_domain,
			   "external ReplayGain database configured but not initialized: {}",
			   replay_gain_external_db_path);
}

static bool
ColumnIsNull(sqlite3_stmt *stmt, int column) noexcept
{
	return sqlite3_column_type(stmt, column) == SQLITE_NULL;
}

static float
ColumnFloat(sqlite3_stmt *stmt, int column) noexcept
{
	return static_cast<float>(sqlite3_column_double(stmt, column));
}

bool
replay_gain_external_enabled() noexcept
{
	return !replay_gain_external_db_path.empty();
}

bool
replay_gain_external_read(std::string_view uri, ReplayGainInfo &info) noexcept
{
	if (replay_gain_external_db_path.empty())
		return false;

	if (uri.empty()) {
		LogDebug(external_replay_gain_domain,
			 "external ReplayGain lookup skipped: empty URI");
		return false;
	}

	FmtDebug(external_replay_gain_domain,
		 "external ReplayGain lookup: {}", uri);

	SqliteDb db;
	if (sqlite3_open_v2(replay_gain_external_db_path.c_str(), &db.db,
			    SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
			    nullptr) != SQLITE_OK) {
		FmtWarning(external_replay_gain_domain,
			 "external ReplayGain database open failed: {}: {}",
			 replay_gain_external_db_path,
			 SqliteError(db.db));
		return false;
	}

	static constexpr char sql[] =
		"SELECT track_gain, track_peak, album_gain, album_peak "
		"FROM replaygain "
		"WHERE uri = ?1";

	SqliteStmt stmt;
	if (sqlite3_prepare_v2(db.db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
		FmtWarning(external_replay_gain_domain,
			 "external ReplayGain query prepare failed: {}",
			 SqliteError(db.db));
		return false;
	}

	if (sqlite3_bind_text(stmt.stmt, 1, uri.data(),
		      static_cast<int>(uri.size()),
		      SQLITE_TRANSIENT) != SQLITE_OK) {
		FmtWarning(external_replay_gain_domain,
			 "external ReplayGain query bind failed: {}",
			 SqliteError(db.db));
		return false;
	}

	const int step_result = sqlite3_step(stmt.stmt);
	if (step_result != SQLITE_ROW) {
		if (step_result == SQLITE_DONE)
			FmtInfo(external_replay_gain_domain,
				 "external ReplayGain miss: {}", uri);
		else
			FmtWarning(external_replay_gain_domain,
				 "external ReplayGain query failed: {}: {}",
				 uri, SqliteError(db.db));

		return false;
	}

	info = ReplayGainInfo::Undefined();

	if (!ColumnIsNull(stmt.stmt, 0)) {
		info.track.gain = ColumnFloat(stmt.stmt, 0);
		info.track.peak = ColumnIsNull(stmt.stmt, 1)
			? 0.0f
			: ColumnFloat(stmt.stmt, 1);
	}

	if (!ColumnIsNull(stmt.stmt, 2)) {
		info.album.gain = ColumnFloat(stmt.stmt, 2);
		info.album.peak = ColumnIsNull(stmt.stmt, 3)
			? 0.0f
			: ColumnFloat(stmt.stmt, 3);
	}

	if (!info.IsDefined()) {
		FmtDebug(external_replay_gain_domain,
			 "external ReplayGain row ignored without gain values: {}",
			 uri);
		return false;
	}

	FmtInfo(external_replay_gain_domain,
		 "external ReplayGain hit: {}: track_gain={} track_peak={} album_gain={} album_peak={}",
		 uri,
		 info.track.gain,
		 info.track.peak,
		 info.album.gain,
		 info.album.peak);

	return true;
}