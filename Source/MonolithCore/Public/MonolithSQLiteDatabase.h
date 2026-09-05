#pragma once

#include "SQLiteDatabase.h"
#include "Misc/AssertionMacros.h"

// Callers include SQLiteCore in their module dependencies. Keep this guard in
// the plugin so installed engines need no source patch. Finalize every prepared
// statement before Close; a failed close retains the handle for a later retry.
class FMonolithSQLiteDatabase : public FSQLiteDatabase
{
public:
	bool Close()
	{
		if (!IsValid()) return true;
		const bool bClosed = FSQLiteDatabase::Close();
		ensureAlwaysMsgf(bClosed,
			TEXT("Monolith SQLite Close failed: finalize all outstanding statements before closing the database"));
		return bClosed;
	}
};
