// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// WriteBehindStore.cpp — batched position/HP writes (M5 area E, §15).

#include "pch.h"
#include "WriteBehindStore.h"

namespace Neuron::Persist
{

std::optional<int64_t> WriteBehindStore::FlushBatch(std::span<const WriteBehindRow> rows)
{
    if (rows.empty())
        return int64_t{ 0 };

    auto lease = m_pool->Acquire();
    if (!lease)
        return std::nullopt;

    // One transaction for the whole batch so the RPO checkpoint is all-or-nothing: on a
    // failure mid-batch nothing lands and the watermark (PersistenceThread) does not
    // advance, so the next cadence re-flushes. NOTE: this carries NO economy effect —
    // position/HP/state only (§15 separation invariant).
    if (!lease->BeginTransaction())
        return std::nullopt;

    int64_t written = 0;
    for (const auto& r : rows) {
        const SqlParam p[] = {
            SqlParam::Make(r.x), SqlParam::Make(r.y), SqlParam::Make(r.z),
            SqlParam::Make(static_cast<int64_t>(r.shieldHp)),
            SqlParam::Make(static_cast<int64_t>(r.armorHp)),
            SqlParam::Make(static_cast<int64_t>(r.hullHp)),
            SqlParam::Make(static_cast<int64_t>(r.stateByte)),
            SqlParam::Make(r.dbId),
        };
        // Bases and Ships share the layered-HP columns; BaseState/ShipState is the
        // state byte. (Ships has no SafeZone/Cargo* here — those are area-D writes.)
        const char* sql =
            (r.entity == WriteBehindEntity::Base)
            ? "UPDATE Bases SET PosX=?, PosY=?, PosZ=?, ShieldHp=?, ArmorHp=?, HullHp=?, "
              "BaseState=?, UpdatedAt=SYSUTCDATETIME() WHERE BaseId=?"
            : "UPDATE Ships SET PosX=?, PosY=?, PosZ=?, ShieldHp=?, ArmorHp=?, HullHp=?, "
              "ShipState=?, UpdatedAt=SYSUTCDATETIME() WHERE ShipId=?";
        auto n = lease->ExecNonQuery(sql, p);
        if (!n) {
            (void)lease->Rollback();
            return std::nullopt;
        }
        written += *n;
    }

    if (!lease->Commit()) {
        (void)lease->Rollback();
        return std::nullopt;
    }
    return written;
}

} // namespace Neuron::Persist
