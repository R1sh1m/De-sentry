import type { DataDirCandidate } from "../api.js";
import { sidecar } from "../bridge.js";
import { refreshNodeList, refreshTopology, store, type NodeView } from "../state.js";
import { bytes, count, displayNodeName, engineLabel } from "./format.js";
import { openDeleteNodeModal } from "../views/deleteNodeModal.js";

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * Prompts user to delete or remove a supervised node with the exact requested format:
 * "The selected node {node name} has {amount of data} in it related to {its data description}, are you sure you want to delete this?"
 */
export function promptDeleteSupervisedNode(node: NodeView): void {
  const nodeName = displayNodeName(node.process.node_name, node.process.data_dir, node.process.node_id);
  const collections = node.brain?.collections ?? [];
  const totalDocs = collections.reduce((sum, c) => sum + c.document_count, 0);
  const usedBytes = node.quota?.used_bytes ?? 0;

  let amountOfData = bytes(usedBytes);
  if (totalDocs > 0) {
    amountOfData += ` (${count(totalDocs)} document${totalDocs === 1 ? "" : "s"} across ${collections.length} collection${collections.length === 1 ? "" : "s"})`;
  } else if (usedBytes === 0) {
    amountOfData = "0 bytes (unwritten)";
  }

  let dataDescription = "its configured storage engines and cryptographic ledger";
  if (collections.length > 0) {
    dataDescription = `collections: ${collections.map((c) => `${c.name} [${engineLabel(c.engine)}]`).join(", ")}`;
  }

  openDeleteNodeModal({
    nodeName,
    amountOfData,
    dataDescription,
    allowForgetOnly: true,
    onConfirm: async (deleteData: boolean) => {
      try {
        await sidecar.deleteNode(node.process.node_id, deleteData);
        store.toast(
          "success",
          deleteData ? "Node deleted" : "Node forgotten",
          `${nodeName} was removed ${deleteData ? "and its files were deleted" : "from the app"}.`,
        );
        await refreshNodeList();
        await refreshTopology();
      } catch (err) {
        store.toast("error", `Could not delete ${nodeName}`, describeError(err));
        throw err;
      }
    },
  });
}

/**
 * Prompts user to delete an unadopted candidate directory found in Step 1 of Wizard:
 */
export function promptDeleteCandidateNode(candidate: DataDirCandidate, onDeleted?: () => void): void {
  const nodeName = candidate.node_name || candidate.path.split(/[\\/]/).filter(Boolean).pop() || "existing-node";
  const amountOfData = "existing ledger state and catalog files";
  const dataDescription = `previous node configuration at ${candidate.path}`;

  openDeleteNodeModal({
    nodeName,
    amountOfData,
    dataDescription,
    allowForgetOnly: false,
    onConfirm: async () => {
      try {
        await sidecar.deleteDirectory(candidate.path);
        store.toast("success", "Node folder deleted", `${candidate.path} was deleted from disk.`);
        if (onDeleted) onDeleted();
      } catch (err) {
        store.toast("error", `Could not delete ${nodeName}`, describeError(err));
        throw err;
      }
    },
  });
}
