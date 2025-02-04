// Copyright (c) YugaByte, Inc
package com.yugabyte.yw.models.filters;

import com.yugabyte.yw.models.Restore;
import java.util.Date;
import java.util.Set;
import java.util.UUID;
import lombok.Builder;
import lombok.NonNull;
import lombok.Value;

@Value
@Builder(toBuilder = true)
public class RestoreFilter {

  Date dateRangeStart;
  Date dateRangeEnd;
  Set<Restore.State> states;
  Set<String> universeNameList;
  Set<String> sourceUniverseNameList;
  Set<UUID> storageConfigUUIDList;
  Set<UUID> universeUUIDList;
  UUID customerUUID;
  boolean onlyShowDeletedSourceUniverses;
}
