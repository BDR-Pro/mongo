/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/exec/update_stage.h"

namespace mongo {

struct TimeseriesModifyParams {
    TimeseriesModifyParams(const DeleteStageParams* deleteParams)
        : isUpdate(false),
          isMulti(deleteParams->isMulti),
          fromMigrate(deleteParams->fromMigrate),
          isExplain(deleteParams->isExplain),
          stmtId(deleteParams->stmtId),
          canonicalQuery(deleteParams->canonicalQuery) {}

    TimeseriesModifyParams(const UpdateStageParams* updateParams)
        : isUpdate(true),
          isMulti(updateParams->request->isMulti()),
          fromMigrate(updateParams->request->source() == OperationSource::kFromMigrate),
          isExplain(updateParams->request->explain()),
          canonicalQuery(updateParams->canonicalQuery),
          isFromOplogApplication(updateParams->request->isFromOplogApplication()),
          updateDriver(updateParams->driver) {
        tassert(7314203,
                "timeseries updates should only have one stmtId",
                updateParams->request->getStmtIds().size() == 1);
        stmtId = updateParams->request->getStmtIds().front();
    }

    // Is this an update or a delete operation?
    bool isUpdate;

    // Is this a multi update/delete?
    bool isMulti;

    // Is this command part of a migrate operation that is essentially like a no-op when the
    // cluster is observed by an external client.
    bool fromMigrate;

    // Are we explaining a command rather than actually executing it?
    bool isExplain;

    // The stmtId for this particular command.
    StmtId stmtId = kUninitializedStmtId;

    // The parsed query predicate for this command. Not owned here.
    CanonicalQuery* canonicalQuery;

    // True if this command was triggered by the application of an oplog entry.
    bool isFromOplogApplication;

    // Contains the logic for applying mods to documents. Only present for updates. Not owned. Must
    // outlive the TimeseriesModifyStage.
    UpdateDriver* updateDriver;
};

/**
 * Unpacks time-series bucket documents and writes the modified documents.
 *
 * The stage processes one bucket at a time, unpacking all the measurements and writing the output
 * bucket in a single doWork() call.
 */
class TimeseriesModifyStage final : public RequiresMutableCollectionStage {
public:
    static const char* kStageType;

    TimeseriesModifyStage(ExpressionContext* expCtx,
                          TimeseriesModifyParams&& params,
                          WorkingSet* ws,
                          std::unique_ptr<PlanStage> child,
                          const CollectionPtr& coll,
                          BucketUnpacker bucketUnpacker,
                          std::unique_ptr<MatchExpression> residualPredicate);

    StageType stageType() const {
        return STAGE_TIMESERIES_MODIFY;
    }

    bool isEOF() final;

    std::unique_ptr<PlanStageStats> getStats();

    const SpecificStats* getSpecificStats() const {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id);

    bool containsDotsAndDollarsField() const {
        return _params.isMulti && _params.updateDriver->containsDotsAndDollarsField();
    }

protected:
    void doSaveStateRequiresCollection() final {
        _preWriteFilter.saveState();
    }

    void doRestoreStateRequiresCollection() final;

private:
    bool _isMultiWrite() const {
        return _params.isMulti;
    }

    bool _isSingletonWrite() const {
        return !_isMultiWrite();
    }

    /**
     * Writes the modifications to a bucket.
     */
    PlanStage::StageState _writeToTimeseriesBuckets(
        WorkingSetID bucketWsmId,
        const std::vector<BSONObj>& unchangedMeasurements,
        const std::vector<BSONObj>& modifiedMeasurements,
        bool bucketFromMigrate);

    /**
     * Helper to set up state to retry 'bucketId' after yielding and establishing a new storage
     * snapshot.
     */
    void _retryBucket(WorkingSetID bucketId);

    template <typename F>
    std::pair<boost::optional<PlanStage::StageState>, bool> _checkIfWritingToOrphanedBucket(
        ScopeGuard<F>& bucketFreer, WorkingSetID id);

    /**
     * Gets the next bucket to process.
     */
    PlanStage::StageState _getNextBucket(WorkingSetID& id);

    TimeseriesModifyParams _params;

    WorkingSet* _ws;

    //
    // Main execution machinery data structures.
    //

    BucketUnpacker _bucketUnpacker;

    // Determines the measurements to modify in this bucket, and by inverse, those to keep
    // unmodified. This predicate can be null if we have a meta-only or empty predicate on singleton
    // deletes or updates.
    std::unique_ptr<MatchExpression> _residualPredicate;

    /**
     * This member is used to check whether the write should be performed, and if so, any other
     * behavior that should be done as part of the write (e.g. skipping it because it affects an
     * orphan document). A yield cannot happen between the check and the write, so the checks are
     * embedded in the stage.
     *
     * It's refreshed after yielding and reacquiring the locks.
     */
    write_stage_common::PreWriteFilter _preWriteFilter;

    TimeseriesModifyStats _specificStats{};

    // A pending retry to get to after a NEED_YIELD propagation and a new storage snapshot is
    // established. This can be set when a write fails or when a fetch fails.
    WorkingSetID _retryBucketId = WorkingSet::INVALID_ID;
};
}  //  namespace mongo
