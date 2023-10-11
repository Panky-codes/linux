large-block-minorder-6.6.0-rc5-v3 (2024-01-22):
- Added a new helper min_nrpages and added CONFIG_THP for enabling
  mapping_large_folio_support. PATCH 1
- Used the helper instead of open coding min_nrpages. Used the order
  instead of calling mapping_min_folio_order(). Patch 2
- Combined Patch 3 and patch 4 that adds some VM_BUG_ON
- Patch 5: Use the helper instead of calculating the nr pages from
  minorder
- Patch 6: Use the helper and rename some var
- Patch 8: Used the helper in ra_state_init
- Patch 10: Used the helper in ra_unbounded.
- Patch 11: Used the helper in force_page_cache and used IS_ALIGNED
  instead of opencoding the alignment
- Merged get_init|next_ra patches and added the ondemand_changes. Used
  !IS_ALIGNED and changed get_init|next_ra functions to take min_nrpages
  directly. It should be PATCH14
- Don't split a folio if it has minorder set. Remove the old code where
  we set extra pins if it has that requirement. PATCH 15
- Split the code in XFS between the validation of mapping count. Put the
  icache code changes with enabling bs > ps.
- Added CONFIG_XFS_LBS option

large-block-minorder-6.6.0-rc5-v2 (2023-10-30):
- align the index in do_read_cache_folio()

large-block-minorder-6.6.0-rc5-v1 (2023-10-23):
- Removed truncate changes
- Fixed generic/091 with iomap changes to iomap_dio_zero function.

large-block-minorder-6.6.0-rc5:
- Took care of folio truncation scenario in page_cache_ra_unbounded()
  that happens after read_pages if a folio was found.
- invalidate_inode_pages2_range() rounded_up the start instead of
  rounding down. End index was rounded down instead of rounding_up. end
  is subtracted with 1 to make sure it points to the last folio.
