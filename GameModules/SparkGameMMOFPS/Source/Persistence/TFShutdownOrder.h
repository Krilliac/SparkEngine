/** @file TFShutdownOrder.h @brief Pins the data-loss-sensitive shutdown dependency. */
#pragma once

namespace Terrafront
{
    template <typename Social, typename Outfit, typename Progression, typename Region>
    bool CheckpointPersistenceBeforeTeardown(Social& social, Outfit& outfit, Progression& progression, Region& region)
    {
        // Run every checkpoint even after one fails so independent stores have
        // the best chance to become durable. The caller must not tear down any
        // dependency unless the aggregate succeeds.
        const bool socialOk = social.Checkpoint();
        const bool outfitOk = outfit.Checkpoint();
        const bool progressionOk = progression.Checkpoint();
        const bool regionOk = region.Checkpoint();
        return socialOk && outfitOk && progressionOk && regionOk;
    }

    template <typename Progression, typename Database>
    bool FlushProgressionThenCloseDatabase(Progression& progression, Database& database)
    {
        if (!progression.Shutdown())
            return false;
        return database.Close();
    }
} // namespace Terrafront
