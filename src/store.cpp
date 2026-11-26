#include"store.hpp"

Store::Store(const std::string &path)
    : db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE, 5000)
{
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA synchronous=NORMAL");
    db.exec("CREATE TABLE IF NOT EXISTS counter(id INTEGER PRIMARY KEY, value INTEGER NOT NULL)");
    db.exec("INSERT OR IGNORE INTO counter VALUES(1, 0)");
}

std::int64_t Store::read()
{
    std::lock_guard<std::mutex> lock(m);
    return db.execAndGet("SELECT value FROM counter WHERE id = 1").getInt64();
}

void Store::add(std::int64_t delta)
{
    //critical section
    std::lock_guard<std::mutex> lock(m);
    SQLite::Statement q(db, "UPDATE counter SET value = value + ? WHERE id = 1");
    q.bind(1, delta);
    q.exec();
    writes++;
}
