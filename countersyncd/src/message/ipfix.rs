use std::sync::Arc;

pub type IPFixTemplates = Arc<Vec<u8>>;
pub type IPFixTemplatesMessage = (String, IPFixTemplates);
