#include "config.h"
#include "ExternalReplayGain.hxx"

#include "config/Data.hxx"
#include "config/Option.hxx"
#include "fs/AllocatedPath.hxx"
#include "input/InputStream.hxx"

#include <sqlite3.h>

#include <string>

static std::string replay_gain_external_db_path;

void
replay_gain_external_init(const ConfigData &config)
{
	const auto path = config.GetPath(ConfigOption::REPLAYGAIN_EXTERNAL_DB);

	if (path.IsNull())
		replay_gain_external_db_path.clear();
	else
		replay_gain_external_db_path = path.c_str();
}

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
replay_gain_external_read(InputStream &is, ReplayGainInfo &info) noexcept
{
	if (replay_gain_external_db_path.empty())
		return false;

	const char *const uri = is.GetURI();
	if (uri == nullptr || *uri == 0)
		return false;

	SqliteDb db;
	if (sqlite3_open_v2(replay_gain_external_db_path.c_str(), &db.db,
			    SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
			    nullptr) != SQLITE_OK)
		return false;

	static constexpr char sql[] =
		"SELECT track_gain, track_peak, album_gain, album_peak "
		"FROM replaygain "
		"WHERE uri = ?1";

	SqliteStmt stmt;
	if (sqlite3_prepare_v2(db.db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK)
		return false;

	if (sqlite3_bind_text(stmt.stmt, 1, uri, -1, SQLITE_TRANSIENT) != SQLITE_OK)
		return false;

	if (sqlite3_step(stmt.stmt) != SQLITE_ROW)
		return false;

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

	return info.IsDefined();
}