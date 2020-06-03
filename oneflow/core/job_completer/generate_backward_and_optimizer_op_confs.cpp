#include "oneflow/core/job_completer/op_graph_pass.h"
#include "oneflow/core/job_completer/autograd.h"
#include "oneflow/core/job_completer/optimizer.h"

namespace oneflow {

namespace {

void UpdateJobHelperConfProducedLbi2ConsumedDiffLbi(
    const HashMap<LogicalBlobId, LogicalBlobId>& lbi2diff_lbi, JobBuilder* job_builder) {
  auto& mut_pairs =
      (*job_builder->mutable_helper()->mutable_tag2lbi_relations())[kProducedLbi2ConsumedDiffLbi];
  for (const auto& pair : lbi2diff_lbi) {
    auto* mut_pair = mut_pairs.add_pair();
    *mut_pair->mutable_first() = pair.first;
    *mut_pair->mutable_second() = pair.second;
  }
}

void BindIdenticalSbpObaPairsBetweenIbns(const OpNode& op_node, JobBuilder* job_builder) {
  HashMap<LogicalBlobId, std::vector<OpBlobArg>> in_lbi2obas;
  for (const std::string& ibn : op_node.op().input_bns()) {
    in_lbi2obas[op_node.op().BnInOp2Lbi(ibn)].push_back(GenOpBlobArg(op_node.op().op_name(), ibn));
  }
  for (const auto& pair : in_lbi2obas) {
    if (pair.second.size() > 1) {
      FOR_RANGE(int32_t, i, 1, pair.second.size()) {
        job_builder->BindIdenticalSbpOpBlobArgPair(pair.second.at(0), pair.second.at(i));
      }
    }
  }
}

void SetSbpSignatureHintByIdenticalSbpObaPairs(const OpGraph& op_graph, JobBuilder* job_builder) {
  HashMap<OpBlobArg, const SbpParallel*> oba2sbp_parallel;
  op_graph.ForEachNode([&](OpNode* op_node) {
    auto ForEachBn = [&](const std::function<void(const std::string&)>& Handler) {
      for (const auto& ibn : op_node->op().input_bns()) { Handler(ibn); }
      for (const auto& obn : op_node->op().output_bns()) { Handler(obn); }
    };
    ForEachBn([&](const std::string& bn_in_op) {
      const auto& oba = GenOpBlobArg(op_node->op().op_name(), bn_in_op);
      oba2sbp_parallel[oba] = &op_node->SbpParallel4Lbi(op_node->op().BnInOp2Lbi(bn_in_op));
    });
  });
  auto HasSbpParallel = [&](const OpBlobArg& oba) {
    return oba2sbp_parallel.find(oba) != oba2sbp_parallel.end();
  };
  for (const auto& pair : job_builder->job().helper().identical_sbp_oba_pairs().pair()) {
    const SbpParallel* sbp_parallel = nullptr;
    if (HasSbpParallel(pair.first()) && HasSbpParallel(pair.second())) {
      CHECK(oba2sbp_parallel.at(pair.first()) == oba2sbp_parallel.at(pair.second()));
      sbp_parallel = oba2sbp_parallel.at(pair.first());
    } else if (HasSbpParallel(pair.first())) {
      sbp_parallel = oba2sbp_parallel.at(pair.first());
    } else if (HasSbpParallel(pair.second())) {
      sbp_parallel = oba2sbp_parallel.at(pair.second());
    } else {
      UNIMPLEMENTED();
    }
    *job_builder->MutSbpParallel4Oba(pair.first()) = *sbp_parallel;
    *job_builder->MutSbpParallel4Oba(pair.second()) = *sbp_parallel;
  }
}

void UpdateOpSbpSignatureHint(const OpGraph& op_graph, JobBuilder* job_builder) {
  op_graph.ForEachNode(
      [&](OpNode* op_node) { BindIdenticalSbpObaPairsBetweenIbns(*op_node, job_builder); });
  SetSbpSignatureHintByIdenticalSbpObaPairs(op_graph, job_builder);
}

class GenerateBackwardAndOptimizerOpConfs final : public OpGraphPass {
 public:
  bool IsEnabled() const override { return GlobalJobDesc().IsTrain(); }
  OF_DISALLOW_COPY_AND_MOVE(GenerateBackwardAndOptimizerOpConfs);
  GenerateBackwardAndOptimizerOpConfs() = default;
  ~GenerateBackwardAndOptimizerOpConfs() override = default;

  void Apply(const OpGraph& op_graph, JobBuilder* job_builder) const override;
};

void FilterModelLbi2DiffLbi(const OpGraph& op_graph,
                            const HashMap<LogicalBlobId, LogicalBlobId>& lbi2diff_lbi,
                            HashMap<LogicalBlobId, LogicalBlobId>* model_lbi2model_diff_lbi) {
  for (const auto& pair : lbi2diff_lbi) {
    const LogicalBlobId& lbi = pair.first;
    const LogicalBlobId& diff_lbi = pair.second;
    const OpNode* producer = op_graph.OpNode4OpName(lbi.op_name());
    if (producer->op().op_conf().has_variable_conf()) {
      (*model_lbi2model_diff_lbi)[lbi] = diff_lbi;
    }
  }
}

void GenerateBackwardAndOptimizerOpConfs::Apply(const OpGraph& op_graph,
                                                JobBuilder* job_builder) const {
  TeePersistentLogStream::Create("PreGenerateBackwardAndOptimizerOpConfs.prototxt")
      ->Write(job_builder->job().DebugString());
  LogicalBlobId total_loss_instance_num;
  HashMap<LogicalBlobId, LogicalBlobId> lbi2diff_lbi;
  AutoGrad(op_graph, job_builder, &lbi2diff_lbi);
  HashMap<LogicalBlobId, LogicalBlobId> model_lbi2model_diff_lbi;
  FilterModelLbi2DiffLbi(op_graph, lbi2diff_lbi, &model_lbi2model_diff_lbi);
  AddDiffParallelCast(op_graph, job_builder, &model_lbi2model_diff_lbi);
  ScaleModelDiffByLossInstanceNum(op_graph, job_builder, &model_lbi2model_diff_lbi);
  ScaleModelDiffByLossScale(op_graph, job_builder, &model_lbi2model_diff_lbi);
  const NormalModelUpdateOpUserConf& model_update_conf =
      job_builder->job().job_conf().train_conf().model_update_conf();
  RegularizeGradient(op_graph, job_builder, &model_lbi2model_diff_lbi);
  if (model_update_conf.has_clip_conf()) {
    ClipGradient(op_graph, job_builder, &model_lbi2model_diff_lbi, model_update_conf.clip_conf());
  }
  AddOptimizerOpConf(op_graph, job_builder, model_lbi2model_diff_lbi);
  UpdateJobHelperConfProducedLbi2ConsumedDiffLbi(lbi2diff_lbi, job_builder);
  UpdateOpSbpSignatureHint(op_graph, job_builder);
  TeePersistentLogStream::Create("PostGenerateBackwardAndOptimizerOpConfs.prototxt")
      ->Write(job_builder->job().DebugString());
}

REGISTER_FUNCTION_PASS("GenerateBackwardAndOptimizerOpConfs", GenerateBackwardAndOptimizerOpConfs);

}  // namespace

}  // namespace oneflow
