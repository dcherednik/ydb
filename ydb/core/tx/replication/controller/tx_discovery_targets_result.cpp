#include "controller_impl.h"
#include "util.h"

#include <util/string/join.h>

namespace NKikimr::NReplication::NController {

class TController::TTxDiscoveryTargetsResult: public TTxBase {
    TEvPrivate::TEvDiscoveryTargetsResult::TPtr Ev;
    TReplication::TPtr Replication;

public:
    explicit TTxDiscoveryTargetsResult(TController* self, TEvPrivate::TEvDiscoveryTargetsResult::TPtr& ev)
        : TTxBase("TxDiscoveryTargetsResult", self)
        , Ev(ev)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_DISCOVERY_RESULT;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        CLOG_D(ctx, "Execute: " << Ev->Get()->ToString());

        const auto rid = Ev->Get()->ReplicationId;

        Replication = Self->Find(rid);
        if (!Replication) {
            CLOG_W(ctx, "Unknown replication"
                << ": rid# " << rid);
            return true;
        }

        if (Replication->GetState() != TReplication::EState::Ready) {
            CLOG_W(ctx, "Replication state mismatch"
                << ": rid# " << rid
                << ", state# " << Replication->GetState());
            return true;
        }

        NIceDb::TNiceDb db(txc.DB);

        auto fn = [](auto&& arg) -> std::pair<TReplication::ETargetKind, TString>{
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, NYdb::NScheme::TSchemeEntry>) {
                return {TargetKindFromEntryType(arg.Type), arg.Name};
            } else if constexpr (std::is_same_v<T, NYdb::NTable::TIndexDescription>) {
                switch (arg.GetIndexType()) {
                    case NYdb::NTable::EIndexType::GlobalSync:
                    case NYdb::NTable::EIndexType::GlobalUnique:
                        return {TReplication::ETargetKind::GlobalSyncIndex, arg.GetIndexName()};
                    case NYdb::NTable::EIndexType::GlobalAsync: 
                        return {TReplication::ETargetKind::GlobalAsyncIndex, arg.GetIndexName()};
                    default:
                        return {TReplication::ETargetKind::Table, arg.GetIndexName()};
                }
            } else {
                Y_ABORT_UNLESS(false, "unreach");
                //static_assert(false, "non-exhaustive visitor for " + type_name(decltype(arg)));
            }
        };

        if (Ev->Get()->IsSuccess()) {
            for (const auto& target : Ev->Get()->ToAdd) {
                auto [kind, srcPath] = std::visit(fn, target.first);
                const auto& dstPath = target.second;

                const auto tid = Replication->AddTarget(kind, srcPath, dstPath);
                db.Table<Schema::Targets>().Key(rid, tid).Update(
                    NIceDb::TUpdate<Schema::Targets::Kind>(kind),
                    NIceDb::TUpdate<Schema::Targets::SrcPath>(srcPath),
                    NIceDb::TUpdate<Schema::Targets::DstPath>(dstPath)
                );

                CLOG_N(ctx, "Add target"
                    << ": rid# " << rid
                    << ", tid# " << tid
                    << ", kind# " << kind
                    << ", srcPath# " << srcPath
                    << ", dstPath# " << dstPath);
            }
        } else {
            const auto error = JoinSeq(", ", Ev->Get()->Failed);
            Replication->SetState(TReplication::EState::Error, TStringBuilder() << "Discovery error: " << error);

            CLOG_E(ctx, "Discovery error"
                << ": rid# " << rid
                << ", error# " << error);
        }

        db.Table<Schema::Replications>().Key(Replication->GetId()).Update(
            NIceDb::TUpdate<Schema::Replications::State>(Replication->GetState()),
            NIceDb::TUpdate<Schema::Replications::Issue>(Replication->GetIssue()),
            NIceDb::TUpdate<Schema::Replications::NextTargetId>(Replication->GetNextTargetId())
        );

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        CLOG_D(ctx, "Complete");

        if (Replication) {
            Replication->Progress(ctx);
        }
    }

}; // TTxDiscoveryTargetsResult

void TController::RunTxDiscoveryTargetsResult(TEvPrivate::TEvDiscoveryTargetsResult::TPtr& ev, const TActorContext& ctx) {
    Execute(new TTxDiscoveryTargetsResult(this, ev), ctx);
}

}
