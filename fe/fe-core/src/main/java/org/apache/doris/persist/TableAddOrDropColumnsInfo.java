// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.persist;

import org.apache.doris.catalog.Column;
import org.apache.doris.catalog.Index;
import org.apache.doris.common.io.Text;
import org.apache.doris.common.io.Writable;
import org.apache.doris.persist.gson.GsonUtils;

import com.google.gson.annotations.SerializedName;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;

/**
 * PersistInfo for Table properties
 */
public class TableAddOrDropColumnsInfo implements Writable {
    @SerializedName(value = "dbId")
    private long dbId;
    @SerializedName(value = "tableId")
    private long tableId;
    @SerializedName(value = "baseIndexId")
    private long baseIndexId; // only used for ccr, not included in equals
    @SerializedName(value = "indexSchemaMap")
    private Map<Long, LinkedList<Column>> indexSchemaMap;
    @SerializedName(value = "oldIndexSchemaMap")
    private Map<Long, List<Column>> oldIndexSchemaMap; // only used for ccr, not included in equals
    @SerializedName(value = "indexNameToId")
    private Map<String, Long> indexNameToId; // only used for ccr, not included in equals
    @SerializedName(value = "indexes")
    private List<Index> indexes;
    @SerializedName(value = "jobId")
    private long jobId;
    @SerializedName(value = "rawSql")
    private String rawSql;

    public TableAddOrDropColumnsInfo(String rawSql, long dbId, long tableId, long baseIndexId,
            Map<Long, LinkedList<Column>> indexSchemaMap,
            Map<Long, List<Column>> oldIndexSchemaMap,
            Map<String, Long> indexNameToId,
            List<Index> indexes, long jobId) {
        this.rawSql = rawSql;
        this.dbId = dbId;
        this.tableId = tableId;
        this.baseIndexId = baseIndexId;
        this.indexSchemaMap = indexSchemaMap;
        this.oldIndexSchemaMap = oldIndexSchemaMap;
        this.indexNameToId = indexNameToId;
        this.indexes = indexes;
        this.jobId = jobId;
    }

    public long getDbId() {
        return dbId;
    }

    public long getTableId() {
        return tableId;
    }

    public Map<Long, LinkedList<Column>> getIndexSchemaMap() {
        return indexSchemaMap;
    }

    public List<Index> getIndexes() {
        return indexes;
    }

    public long getJobId() {
        return jobId;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        Text.writeString(out, GsonUtils.GSON.toJson(this));
    }

    public static TableAddOrDropColumnsInfo read(DataInput in) throws IOException {
        return GsonUtils.GSON.fromJson(Text.readString(in), TableAddOrDropColumnsInfo.class);
    }

    @Override
    public boolean equals(Object obj) {
        if (obj == this) {
            return true;
        }

        if (!(obj instanceof TableAddOrDropColumnsInfo)) {
            return false;
        }

        TableAddOrDropColumnsInfo info = (TableAddOrDropColumnsInfo) obj;

        return (dbId == info.dbId && tableId == info.tableId
                && indexSchemaMap.equals(info.indexSchemaMap) && indexes.equals(info.indexes)
                && jobId == info.jobId);
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder();
        sb.append(" dbId: ").append(dbId);
        sb.append(" tableId: ").append(tableId);
        sb.append(" indexSchemaMap: ").append(indexSchemaMap);
        sb.append(" oldIndexSchemaMap: ").append(oldIndexSchemaMap);
        sb.append(" indexes: ").append(indexes);
        sb.append(" jobId: ").append(jobId);
        return sb.toString();
    }

    public String toJson() {
        return GsonUtils.GSON.toJson(this);
    }
}
