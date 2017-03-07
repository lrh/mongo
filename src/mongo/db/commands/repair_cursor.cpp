// repair_cursor.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"

namespace mongo {

using std::string;

class RepairCursorCmd : public Command {
public:
    RepairCursorCmd() : Command("repairCursor") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        Privilege p(parseResourcePattern(dbname, cmdObj), actions);
        if (AuthorizationSession::get(client)->isAuthorizedForPrivilege(p))
            return Status::OK();
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) {
        NamespaceString ns(parseNs(dbname, cmdObj));

        AutoGetCollectionForRead ctx(opCtx, ns);

        Collection* collection = ctx.getCollection();
        if (!collection) {
            return appendCommandStatus(
                result, Status(ErrorCodes::NamespaceNotFound, "ns does not exist: " + ns.ns()));
        }

        auto cursor = collection->getRecordStore()->getCursorForRepair(opCtx);
        if (!cursor) {
            return appendCommandStatus(
                result, Status(ErrorCodes::CommandNotSupported, "repair iterator not supported"));
        }

        std::unique_ptr<WorkingSet> ws(new WorkingSet());
        std::unique_ptr<MultiIteratorStage> stage(
            new MultiIteratorStage(opCtx, ws.get(), collection));
        stage->addIterator(std::move(cursor));

        auto statusWithPlanExecutor = PlanExecutor::make(
            opCtx, std::move(ws), std::move(stage), collection, PlanExecutor::YIELD_AUTO);
        invariant(statusWithPlanExecutor.isOK());
        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // 'exec' will be used in getMore(). It was automatically registered on construction
        // due to the auto yield policy, so it could yield during plan selection. We deregister
        // it now so that it can be registed with ClientCursor.
        exec->deregisterExec();
        exec->saveState();
        exec->detachFromOperationContext();

        auto pinnedCursor = collection->getCursorManager()->registerCursor(
            {exec.release(),
             ns.ns(),
             opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot()});

        appendCursorResponseObject(
            pinnedCursor.getCursor()->cursorid(), ns.ns(), BSONArray(), &result);

        return true;
    }
} repairCursorCmd;
}
