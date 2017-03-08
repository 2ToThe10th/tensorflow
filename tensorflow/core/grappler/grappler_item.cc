/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/grappler/grappler_item.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/variable.pb.h"
#include "tensorflow/core/framework/versions.h"
#include "tensorflow/core/grappler/inputs/utils.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"
#include "tensorflow/core/public/version.h"

namespace tensorflow {
namespace grappler {

// static
std::unique_ptr<GrapplerItem> GrapplerItem::FromMetaGraphDef(
    const string& id, const MetaGraphDef& meta_graph, const ItemConfig& cfg) {
  // Check if the graph is compatible with the current version of TensorFlow.
  Status status =
      CheckVersions(meta_graph.graph_def().versions(), TF_GRAPH_DEF_VERSION,
                    TF_GRAPH_DEF_VERSION_MIN_PRODUCER, "GraphDef", "graph");
  if (!status.ok()) {
    LOG(ERROR) << "Cannot process item: " << status.error_message();
    return nullptr;
  }

  std::unique_ptr<GrapplerItem> new_item(new GrapplerItem());
  if (id.empty()) {
    LOG(ERROR) << "id must be non-empty.";
    return nullptr;
  }
  new_item->id = id;
  new_item->graph = meta_graph.graph_def();

  // Attempt to detect the fetch node(s).
  if (meta_graph.collection_def().count("train_op") > 0) {
    const CollectionDef& nodes = meta_graph.collection_def().at("train_op");
    if (nodes.has_node_list()) {
      for (const auto& node : nodes.node_list().value()) {
        const string name = NodeName(node);
        if (name.empty()) {
          LOG(ERROR) << "Invalid fetch node name " << node
                     << ", skipping this input";
          return nullptr;
        }
        LOG(INFO) << "Will use fetch node " << name;
        new_item->fetch.push_back(name);
      }
    }
  }
  if (new_item->fetch.empty()) {
    LOG(ERROR) << "Failed to detect the fetch node(s), skipping this input";
    return nullptr;
  }

  for (auto& node : *new_item->graph.mutable_node()) {
    // Delete user specified placement if requested.
    if (cfg.ignore_user_placement) {
      node.clear_device();
    }

    if (node.op() == "Placeholder" || node.op() == "PlaceholderV2") {
      if (node.attr().count("dtype") == 0) {
        LOG(ERROR) << "Unknown type for placeholder " << node.name()
                   << ", skipping this input";
        return nullptr;
      }
      DataType type = node.attr().at("dtype").type();

      if (node.attr().count("shape") == 0) {
        LOG(INFO) << "Unknown shape for placeholder " << node.name()
                  << ", skipping this input";
        return nullptr;
      }
      TensorShape shape(node.attr().at("shape").shape());
      // Some placeholder nodes have a mis-match between the node
      // attribute "shape" and a different node attribute "_output_shapes".
      // Specifically, a shape with shape.dims() == 0 could indicate either
      // a scalar or an unknown shape. In those cases, we check _output_shapes
      // for additional information.
      // This case is observed in the bnmt graphs. Have not observed any
      // cases where there was more than 1 _output_shapes, so limit it
      // to cases where there is only 1 _output_shapes.
      // We only do this if cfg.placeholder_unknown_output_shape_dim has
      // been set to avoid crashing non-BNMT graphs.
      if ((cfg.placeholder_unknown_output_shape_dim >= 0) &&
          (shape.dims() == 0) && (node.attr().count("_output_shapes") == 1) &&
          (node.attr().at("_output_shapes").list().shape(0).dim_size() != 0)) {
        shape.Clear();
        for (int dim_i = 0;
             dim_i <
             node.attr().at("_output_shapes").list().shape(0).dim_size();
             dim_i++) {
          const ::tensorflow::TensorShapeProto_Dim dim =
              node.attr().at("_output_shapes").list().shape(0).dim(dim_i);
          if (dim.size() == -1) {
            shape.AddDim(cfg.placeholder_unknown_output_shape_dim);
          } else {
            shape.AddDim(node.attr()
                             .at("_output_shapes")
                             .list()
                             .shape(0)
                             .dim(dim_i)
                             .size());
          }
        }
      }
      Tensor fake_input(type, shape);
      // TODO(bsteiner): figure out a better way to initialize the feeds, for
      // example by recording a sample of the fed inputs in mldash when running
      // the graph.
      memset(const_cast<char*>(fake_input.tensor_data().data()), 0,
             fake_input.tensor_data().size());
      new_item->feed.emplace_back(node.name(), fake_input);
    }

    if (cfg.ignore_colocation) {
      auto attr = node.mutable_attr();
      auto it = attr->find("_class");
      if (it != attr->end()) {
        attr->erase(it);
      }
    }
  }

  for (const string& var_collection :
       {"variables", "local_variables", "model_variables",
        "trainable_variables"}) {
    if (meta_graph.collection_def().count(var_collection) == 0) {
      continue;
    }
    const CollectionDef& vars = meta_graph.collection_def().at(var_collection);
    for (const auto& raw_var : vars.bytes_list().value()) {
      VariableDef var;
      var.ParseFromString(raw_var);
      if (!var.initializer_name().empty()) {
        new_item->init_ops.push_back(var.initializer_name());
      }
    }
  }

  if (meta_graph.collection_def().count("table_initializer") > 0) {
    const CollectionDef& inits =
        meta_graph.collection_def().at("table_initializer");
    if (inits.has_node_list()) {
      for (const auto& node : inits.node_list().value()) {
        new_item->init_ops.push_back(node);
      }
    }
  }

  if (meta_graph.collection_def().count("queue_runners") > 0) {
    const CollectionDef& vars = meta_graph.collection_def().at("queue_runners");
    for (const auto& raw : vars.bytes_list().value()) {
      QueueRunnerDef queue_runner;
      if (!queue_runner.ParseFromString(raw)) {
        LOG(ERROR) << "Could parse queue_runners, skipping this input";
        return nullptr;
      }
      if (queue_runner.cancel_op_name().empty()) {
        LOG(ERROR) << "Queue without a cancel op, skipping this input";
        return nullptr;
      }
      new_item->queue_runners.push_back(queue_runner);
    }
  }

  // Make sure we still can access the input files (aka "asset_filepaths") since
  // these might have been moved or deleted, the cns cell might have been shut
  // down, or we might be running as a user who does not have access to the
  // files.
  if (meta_graph.collection_def().count("asset_filepaths") > 0) {
    const CollectionDef& file_paths =
        meta_graph.collection_def().at("asset_filepaths");
    std::vector<string> paths;
    for (const auto& raw_path : file_paths.bytes_list().value()) {
      paths.push_back(raw_path);
    }
    if (!FilesExist(paths, nullptr)) {
      LOG(ERROR)
          << "Can't access one or more of the asset files, skipping this input";
      return nullptr;
    }
  }

  return new_item;
}

std::vector<const NodeDef*> GrapplerItem::MainOpsFanin() const {
  return ComputeTransitiveFanin(graph, fetch);
}

std::vector<const NodeDef*> GrapplerItem::InitOpsFanin() const {
  return ComputeTransitiveFanin(graph, init_ops);
}

std::vector<const NodeDef*> ComputeTransitiveFanin(
    const GraphDef& graph, const std::vector<string>& terminal_nodes) {
  std::unordered_map<string, const NodeDef*> name_to_node;
  for (const auto& node : graph.node()) {
    name_to_node[node.name()] = &node;
  }

  std::vector<const NodeDef*> queue;
  for (const string& root : terminal_nodes) {
    const NodeDef* node = name_to_node[NodeName(root)];
    CHECK(node);
    queue.push_back(node);
  }

  std::vector<const NodeDef*> result;
  std::unordered_set<const NodeDef*> visited;

  while (!queue.empty()) {
    const NodeDef* node = queue.back();
    queue.pop_back();
    if (!visited.insert(node).second) {
      // The node has already been visited.
      continue;
    }
    result.push_back(node);
    for (const string& input : node->input()) {
      const NodeDef* in = name_to_node[NodeName(input)];
      CHECK(in);
      queue.push_back(in);
    }
  }
  return result;
}

}  // end namespace grappler
}  // end namespace tensorflow
