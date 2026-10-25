#pragma once
#include <functional>
#include <string>
#include <vector>
// Called only inside an existing D1 job process group. Callback runs one stage.
int executePipeline(const std::vector<std::vector<std::string>>& stages,
                    const std::function<int(const std::vector<std::string>&)>& stage);
