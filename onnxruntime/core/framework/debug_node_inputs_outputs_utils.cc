// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef DEBUG_NODE_INPUTS_OUTPUTS

#include "core/framework/debug_node_inputs_outputs_utils.h"

#include <iomanip>
#include <cctype>
#include <sqlite3.h>

#include "core/common/path_utils.h"
#include "core/framework/tensorprotoutils.h"
#include "core/platform/env.h"
#include "core/platform/env_var_utils.h"

namespace onnxruntime {
namespace utils {

namespace {

struct TensorMetadata {

  std::string name;
  std::string producer;
  std::string consumers;
  std::string device;
  int step;
};

bool FilterNode(const NodeDumpOptions& dump_options, const Node& node) {
  auto match_pattern =
      [](const std::string& value, const std::string& delimited_patterns) {
        // match all if empty
        if (delimited_patterns.empty()) return true;

        // search for exact match against delimited patterns
        auto pattern_begin = delimited_patterns.begin();
        while (true) {
          const auto pattern_end = std::find(
              pattern_begin, delimited_patterns.end(), kFilterPatternDelimiter);

          if (std::equal(value.begin(), value.end(), pattern_begin, pattern_end)) return true;

          if (pattern_end == delimited_patterns.end()) break;

          pattern_begin = pattern_end + 1;
        }

        return false;
      };

  return match_pattern(node.Name(), dump_options.filter.name_pattern) &&
         match_pattern(node.OpType(), dump_options.filter.op_type_pattern);
}

std::ostream& operator<<(std::ostream& out, const BFloat16& value) {
  return out << value.ToFloat();
}

std::ostream& operator<<(std::ostream& out, const MLFloat16& value) {
  return out << static_cast<float>(value);
}

template <typename T>
void DumpTensorToStdOut(const Tensor& tensor) {
  const auto& shape = tensor.Shape();
  auto num_items = shape.Size();

  if (num_items == 0) {
    std::cout << "no data";
    return;
  }

  size_t num_dims = shape.NumDimensions();
  size_t num_rows = 1;
  if (num_dims > 1) {
    num_rows = static_cast<size_t>(shape[0]);
  }

  size_t row_size = num_items / num_rows;

  auto data = tensor.DataAsSpan<T>();

  auto print_val = [](const T& value) {
    if (std::is_floating_point<T>::value)
      std::cout << std::setprecision(8) << value;
    else
      std::cout << value;
  };

  for (size_t row = 0; row < num_rows; ++row) {
    print_val(data[row * row_size]);
    for (size_t i = 1; i < row_size; ++i) {
      std::cout << ", ";
      print_val(data[row * row_size + i]);
    }
    std::cout << "\n";
  }

  std::cout << std::endl;
}

PathString MakeTensorFileName(const std::string& tensor_name, const NodeDumpOptions& dump_options) {
  auto make_valid_name = [](std::string name) {
    std::replace_if(
        name.begin(), name.end(), [](char c) { return !std::isalnum(c); }, '_');
    return name;
  };

  return path_utils::MakePathString(make_valid_name(tensor_name), dump_options.file_suffix, ".tensorproto");
}

void DumpTensorToFile(const Tensor& tensor, const std::string& tensor_name, const Path& file_path) {
  auto tensor_proto = utils::TensorToTensorProto(tensor, tensor_name);
  const PathString file_path_str = file_path.ToPathString();
  int output_fd;
  ORT_THROW_IF_ERROR(Env::Default().FileOpenWr(file_path_str, output_fd));
  try {
    ORT_ENFORCE(
        tensor_proto.SerializeToFileDescriptor(output_fd),
        "Failed to write tensor to file - tensor: ", tensor_name, ", file: ", ToMBString(file_path_str));
  } catch (...) {
    ORT_IGNORE_RETURN_VALUE(Env::Default().FileClose(output_fd));
    throw;
  }
  ORT_THROW_IF_ERROR(Env::Default().FileClose(output_fd));
}

bool TensorExistsInSqlDb(int step, std::string name) {

  sqlite3 *db = SqliteConnection();

  std::stringstream ss; 
  ss << "select count(name) from Tensors ";
  ss << "  where name == \"" << name << "\"";
  ss << "  and step == " << step << ";";

  auto callback = [](void* exists, int argc, char** argv, char**) -> int { 
    if (argc > 0 && atoi(argv[0]) > 0) {
      *((bool*)exists) = true;
    }
    return 0; 
  };

  printf("%s\n", ss.str().c_str());
  fflush(stdout);

  const char *error_message = 0;
  bool exists = false;
  int rc = sqlite3_exec(db, ss.str().c_str(), callback, (void*)&exists, (char**)&error_message);
  ORT_ENFORCE(rc == SQLITE_OK,
    "Failed to query existence of tensor ", name, " on step ", step, " on ", error_message);

  return exists;
}

void DumpTensorToSqliteDb(const Tensor& tensor, const TensorMetadata& tensor_metadata) {

  if (TensorExistsInSqlDb(tensor_metadata.step, tensor_metadata.name)) {
    return;
  }

  sqlite3 *db = SqliteConnection();

  std::stringstream ss; 
  ss << "insert into Tensors (step, name, value, device) ";
  ss << "values ( ";
  ss << tensor_metadata.step << ", ";
  ss << "\"" << tensor_metadata.name << "\", ";
  ss << "?, ";
  ss << "\"" << tensor_metadata.device << "\" ";
  ss << ");";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, ss.str().c_str(), -1, &stmt, NULL);
  ORT_ENFORCE(rc == SQLITE_OK, 
    "Failed to prepare sql statement for insertion of tensor ", 
    tensor_metadata.name.c_str(), " into sqlite3 db on ", sqlite3_errmsg(db));

  auto tensor_proto = utils::TensorToTensorProto(tensor, tensor_metadata.name);
  std::string bytes = tensor_proto.SerializeAsString();
  const char* data = bytes.data();
  int size = bytes.size();

  rc = sqlite3_bind_blob(stmt, 1, data, size, SQLITE_STATIC);
  ORT_ENFORCE(rc == SQLITE_OK, 
    "Failed to bind tensor ", tensor_metadata.name.c_str(), 
    " blob for insertion into sqlite3 db on ", sqlite3_errmsg(db));

  rc = sqlite3_step(stmt);
  ORT_ENFORCE(rc == SQLITE_DONE, 
    "Failed to insert tensor ", tensor_metadata.name.c_str(), 
    " into sqlite3 db on ", sqlite3_errmsg(db));

  sqlite3_finalize(stmt);
}


void DumpCpuTensor(
    const NodeDumpOptions& dump_options,
    const Tensor& tensor, const TensorMetadata& tensor_metadata) {
  switch (dump_options.data_destination) {
    case NodeDumpOptions::DataDestination::StdOut: {
      DispatchOnTensorType(tensor.DataType(), DumpTensorToStdOut, tensor);
      break;
    }
    case NodeDumpOptions::DataDestination::TensorProtoFiles: {
      const Path tensor_file = dump_options.output_dir / Path::Parse(MakeTensorFileName(tensor_metadata.name, dump_options));
      DumpTensorToFile(tensor, tensor_metadata.name, tensor_file);
      break;
    }
    case NodeDumpOptions::DataDestination::SqliteDb: {
      DumpTensorToSqliteDb(tensor, tensor_metadata);
      break;
    }
    default:
      ORT_THROW("Unsupported data destination type: ", static_cast<int>(dump_options.data_destination));
  }
}

void DumpTensor(
    const NodeDumpOptions& dump_options,
    const Tensor& tensor, TensorMetadata& tensor_metadata,
    const SessionState& session_state) {
  // check tensor is on CPU before dumping it
  auto& tensor_location = tensor.Location();
  if (tensor_location.device.Type() == OrtDevice::CPU ||
      tensor_location.mem_type == OrtMemTypeCPUInput ||
      tensor_location.mem_type == OrtMemTypeCPUOutput) {
    tensor_metadata.device = "CPU";
    DumpCpuTensor(dump_options, tensor, tensor_metadata);
  } else {
    std::cout << tensor_location << "\n";

#if defined(USE_CUDA) || defined(USE_ROCM)
    const auto data_type = tensor.DataType();
    // Dumping GPU only when cuda is enabled.
    if (tensor_location.device.Type() == OrtDevice::GPU) {
      const auto& execution_providers = session_state.GetExecutionProviders();
      const auto* cpu_execution_provider = execution_providers.Get(onnxruntime::kCpuExecutionProvider);
      auto cpu_allocator = cpu_execution_provider->GetAllocator(0, OrtMemTypeDefault);
      Tensor cpu_tensor{data_type, tensor.Shape(), cpu_allocator};
      const auto& data_transfer_mgr = session_state.GetDataTransferMgr();
      auto status = data_transfer_mgr.CopyTensor(tensor, cpu_tensor);
      if (status == common::Status::OK()) {
        tensor_metadata.device = "GPU";
        DumpCpuTensor(dump_options, cpu_tensor, tensor_metadata);
      } else {
        std::cout << " failed to transfer data to cpu.\n";
      }
    }
#else
    ORT_UNUSED_PARAMETER(session_state);
#endif
  }
}

}  // namespace

const NodeDumpOptions& NodeDumpOptionsFromEnvironmentVariables() {
  static const NodeDumpOptions node_dump_options = []() {
    namespace env_vars = debug_node_inputs_outputs_env_vars;

    NodeDumpOptions opts{};

    // Preserve existing behavior of printing the shapes by default. Turn it off only if the user has requested so
    // explicitly by setting the value of the env variable to 0.
    opts.dump_flags = NodeDumpOptions::DumpFlags::None;
    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpShapeData, true)) {
      opts.dump_flags |= NodeDumpOptions::DumpFlags::Shape;
    }

    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpInputData, false)) {
      opts.dump_flags |= NodeDumpOptions::DumpFlags::InputData;
    }
    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpOutputData, false)) {
      opts.dump_flags |= NodeDumpOptions::DumpFlags::OutputData;
    }

    opts.filter.name_pattern = Env::Default().GetEnvironmentVar(env_vars::kNameFilter);
    opts.filter.op_type_pattern = Env::Default().GetEnvironmentVar(env_vars::kOpTypeFilter);

    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpDataToFiles, false)) {
      opts.data_destination = NodeDumpOptions::DataDestination::TensorProtoFiles;
    }

    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpDataToSqlite, false)) {
      opts.data_destination = NodeDumpOptions::DataDestination::SqliteDb;
    }

    if (ParseEnvironmentVariableWithDefault<bool>(env_vars::kAppendRankToFileName, false)) {
      std::string rank = Env::Default().GetEnvironmentVar("OMPI_COMM_WORLD_RANK");
      if (rank.empty()) {
        opts.file_suffix = "_default_rank_0";
      } else {
        opts.file_suffix = "_rank_" + rank;
      }
    }

    opts.output_dir = Path::Parse(ToPathString(Env::Default().GetEnvironmentVar(env_vars::kOutputDir)));
    opts.sqlite_db_path = Path::Parse(ToPathString(Env::Default().GetEnvironmentVar(env_vars::kSqliteDbPath)));

    // check for confirmation for dumping data to files for all nodes
    const bool is_input_or_output_requested = ((opts.dump_flags & NodeDumpOptions::DumpFlags::InputData) != 0) ||
                                              ((opts.dump_flags & NodeDumpOptions::DumpFlags::OutputData) != 0);

    if (is_input_or_output_requested &&
        opts.data_destination == NodeDumpOptions::DataDestination::TensorProtoFiles &&
        opts.filter.name_pattern.empty() && opts.filter.op_type_pattern.empty()) {
      ORT_ENFORCE(
          ParseEnvironmentVariableWithDefault<bool>(env_vars::kDumpingDataToFilesForAllNodesIsOk, false),
          "The current environment variable configuration will dump node input or output data to files for every node. "
          "This may cause a lot of files to be generated. Set the environment variable ",
          env_vars::kDumpingDataToFilesForAllNodesIsOk, " to confirm this is what you want.");
    }

    return opts;
  }();

  return node_dump_options;
}

sqlite3* SqliteConnection() {

  static std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sqlite_db(
    []() {

      const auto& opt = NodeDumpOptionsFromEnvironmentVariables();
      auto sqlite_db_path = opt.sqlite_db_path.ToPathString();
  
      sqlite3 *db;
      int rc = sqlite3_open(sqlite_db_path.c_str(), &db);
      ORT_ENFORCE(rc == 0, "Failed to connect to sqlite3 db ", sqlite_db_path.c_str());
     
      const char *sql_create_tensor_table = 
        "create table if not exists Tensors ( " \
        "  step int not null, " \
        "  name text not null, " \
        "  shape TensorShapeProto, " \
        "  type TypeProto, "  \
        "  value TensorProto, " \
        "  device text, " \
        "  address int, " \
        "  sizeInBytes int, " \
        "  producer NodeArg, " \
        "  consumers NodeArgList, " \
        "  primary key (step, name) " \
        ");";
        
      const char *error_message = 0;
      rc = sqlite3_exec(db, sql_create_tensor_table, NULL, 0, (char**)&error_message);
      ORT_ENFORCE(rc == SQLITE_OK, 
        "Failed to create Tensors table in sqlite3 db ", sqlite_db_path.c_str(), 
        " on ", error_message);
 
      return db;
    }(), &sqlite3_close);

  return sqlite_db.get();
}

static bool IsAnyOutputDumped(const NodeDumpOptions& dump_options) {
  return dump_options.dump_flags != NodeDumpOptions::DumpFlags::None;
}

static void PrintIf(bool boolean_expression, const std::string& message) {
  if (boolean_expression) {
    std::cout << message;
  }
}

void DumpNodeInputs(
    const NodeDumpOptions& dump_options, 
    const NodeDumpContext& dump_context, 
    const OpKernelContext& context, 
    const Node& node, 
    const SessionState& session_state) {
  const bool is_any_output_dumped = IsAnyOutputDumped(dump_options);
  if (!is_any_output_dumped) {
    return;
  }

  if (!FilterNode(dump_options, node)) return;

  std::cout << "-----------\n";
  std::cout << node.OpType() << " node: " << node.Name() << "\n";

  const auto& input_defs = node.InputDefs();
  TensorMetadata tensor_metadata;

  for (auto i = 0, end = context.InputCount(); i < end; ++i) {
    if (input_defs[i]->Exists()) {
      std::cout << "Input " << i << " Name: " << input_defs[i]->Name();

      const auto* type = context.InputType(i);

      if (type) {
        if (type->IsTensorType()) {
          const auto& tensor = *context.Input<Tensor>(i);
          const auto& shape = tensor.Shape();

          const bool is_shape_set = (dump_options.dump_flags & NodeDumpOptions::DumpFlags::Shape) != 0;
          PrintIf(is_shape_set, MakeString(" Shape: ", shape, "\n"));

          if ((dump_options.dump_flags & NodeDumpOptions::DumpFlags::InputData) != 0) {
	    tensor_metadata.name = input_defs[i]->Name();
	    tensor_metadata.step = dump_context.iteration;
            DumpTensor(dump_options, tensor, tensor_metadata, session_state);
          }
        } else {
          std::cout << " is non-tensor type.\n";
        }
      } else {
        // should never happen...
        std::cout << " was missing data type\n";
      }
    } else {
      std::cout << "Input " << i << " is optional and was not provided.\n";
    }
  }
}

void DumpNodeInputs(
    const NodeDumpContext& dump_context, 
    const OpKernelContext& context, 
    const Node& node, 
    const SessionState& session_state) {
  DumpNodeInputs(NodeDumpOptionsFromEnvironmentVariables(), dump_context, context, node, session_state);
}

void DumpNodeOutputs(
   const NodeDumpOptions& dump_options, 
   const NodeDumpContext& dump_context, 
   OpKernelContext& context, 
   const Node& node, 
   const SessionState& session_state) {
  const bool is_any_output_dumped = IsAnyOutputDumped(dump_options);
  if (!is_any_output_dumped) {
    return;
  }

  if (!FilterNode(dump_options, node)) return;

  std::cout << "-----------\n";
  const auto& output_defs = node.OutputDefs();
  TensorMetadata tensor_metadata;

  for (auto i = 0, end = context.OutputCount(); i < end; ++i) {
    if (output_defs[i]->Exists()) {
      std::cout << "Output " << i << " Name: " << output_defs[i]->Name();

      const auto* type = context.OutputType(i);
      if (type) {
        if (type->IsTensorType()) {
          const auto& tensor = *context.Output<Tensor>(i);
          const auto& shape = tensor.Shape();

          const bool is_shape_set = (dump_options.dump_flags & NodeDumpOptions::DumpFlags::Shape) != 0;
          PrintIf(is_shape_set, MakeString(" Shape: ", shape, "\n"));

          if ((dump_options.dump_flags & NodeDumpOptions::DumpFlags::OutputData) != 0) {
	    tensor_metadata.name = output_defs[i]->Name();
	    tensor_metadata.step = dump_context.iteration;
            DumpTensor(dump_options, tensor, tensor_metadata, session_state);
          }
        } else {
          std::cout << " is non-tensor type.\n";
        }
      } else {
        // should never happen...
        std::cout << "missing data type\n";
      }
    } else {
      std::cout << "Output " << i << " is optional and was not produced.\n";
    }

    std::cout << std::endl;
  }
}

void DumpNodeOutputs(
    const NodeDumpContext& dump_context,
    OpKernelContext& context, 
    const Node& node, 
    const SessionState& session_state) {
  DumpNodeOutputs(NodeDumpOptionsFromEnvironmentVariables(), dump_context, context, node, session_state);
}

}  // namespace utils
}  // namespace onnxruntime

#endif
