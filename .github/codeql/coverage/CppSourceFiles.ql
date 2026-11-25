/**
 * Lists observed C++ translation units under the analyzed source root.
 * The receipt records selected tracked sources and additional observed paths.
 */
import cpp

from File file
where file.fromSource() and file.compiledAsCpp()
select file.getRelativePath() as path
