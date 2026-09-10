# Halo Reach normal checkpoint snapshots

The debug Android launcher can preserve and restore Halo Reach's real
`cache1:\autosave` directory. This is not an emulator save state: it cannot
capture an arbitrary paused frame, CPU state, GPU resources, or renderer
timing. The restored location is the last checkpoint that Halo itself finished
writing.

## Safe snapshot procedure

1. Reach the desired point through normal gameplay.
2. Wait for Halo's checkpoint/autosave indication and storage activity to
   finish.
3. Confirm the checkpoint is stable through normal game behavior.
4. Fully stop XeniOS, cold-start it again, and remain in the launcher.
5. Press **Save Halo checkpoint snapshot** and accept the confirmation.

The snapshot is copied to a unique immutable directory, checksummed with
SHA-256, verified, and then published by a same-filesystem rename. Creating a
new snapshot never replaces an existing snapshot.

## Safe restore procedure

1. Fully stop XeniOS, cold-start it again, and remain in the launcher.
2. Press **Restore Halo checkpoint snapshot**.
3. Select a valid checksummed snapshot and accept the second confirmation.

Before replacement, the current autosave is copied to a new immutable recovery
snapshot. Restore data is copied and checksum-verified in a staging directory.
The current autosave is renamed aside, the staging directory is renamed into
place, and the final checksum is verified. The selected snapshot is never
modified. The original autosave directory is retained as an additional
rollback copy.

After Halo has been launched in an Android process, checkpoint actions are
locked for the rest of that process. This prevents snapshot or restore while
the guest may still own the autosave. Fully stop and restart XeniOS to unlock
them.

The controls are visible only in debug builds. No device installation or
checkpoint operation is part of the source-only test workflow.
