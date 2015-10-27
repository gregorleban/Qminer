(function($) {
    // TODO: make the node ID configurable
    var treeNode = $('#jsdoc-toc-nav');

    // initialize the tree
    treeNode.tree({
        autoEscape: false,
        closedIcon: '&#x21e2;',
        data: [{"label":"<a href=\"module-analytics.html\">analytics</a>","id":"module:analytics","children":[{"label":"<a href=\"module-analytics.HierarchMarkov.html\">HierarchMarkov</a>","id":"module:analytics.HierarchMarkov","children":[]},{"label":"<a href=\"module-analytics.KMeans.html\">KMeans</a>","id":"module:analytics.KMeans","children":[]},{"label":"<a href=\"module-analytics.LogReg.html\">LogReg</a>","id":"module:analytics.LogReg","children":[]},{"label":"<a href=\"module-analytics.NNet.html\">NNet</a>","id":"module:analytics.NNet","children":[]},{"label":"<a href=\"module-analytics.NearestNeighborAD.html\">NearestNeighborAD</a>","id":"module:analytics.NearestNeighborAD","children":[]},{"label":"<a href=\"module-analytics.OneVsAll.html\">OneVsAll</a>","id":"module:analytics.OneVsAll","children":[]},{"label":"<a href=\"module-analytics.PCA.html\">PCA</a>","id":"module:analytics.PCA","children":[]},{"label":"<a href=\"module-analytics.PropHazards.html\">PropHazards</a>","id":"module:analytics.PropHazards","children":[]},{"label":"<a href=\"module-analytics.RecLinReg.html\">RecLinReg</a>","id":"module:analytics.RecLinReg","children":[]},{"label":"<a href=\"module-analytics.RidgeReg.html\">RidgeReg</a>","id":"module:analytics.RidgeReg","children":[]},{"label":"<a href=\"module-analytics.SVC.html\">SVC</a>","id":"module:analytics.SVC","children":[]},{"label":"<a href=\"module-analytics.SVR.html\">SVR</a>","id":"module:analytics.SVR","children":[]},{"label":"<a href=\"module-analytics.Sigmoid.html\">Sigmoid</a>","id":"module:analytics.Sigmoid","children":[]},{"label":"<a href=\"module-analytics-metrics.html\">metrics</a>","id":"module:analytics~metrics","children":[{"label":"<a href=\"module-analytics-metrics.ClassificationScore.html\">ClassificationScore</a>","id":"module:analytics~metrics.ClassificationScore","children":[]},{"label":"<a href=\"module-analytics-metrics.MeanAbsoluteError.html\">MeanAbsoluteError</a>","id":"module:analytics~metrics.MeanAbsoluteError","children":[]},{"label":"<a href=\"module-analytics-metrics.MeanAbsolutePercentageError.html\">MeanAbsolutePercentageError</a>","id":"module:analytics~metrics.MeanAbsolutePercentageError","children":[]},{"label":"<a href=\"module-analytics-metrics.MeanError.html\">MeanError</a>","id":"module:analytics~metrics.MeanError","children":[]},{"label":"<a href=\"module-analytics-metrics.MeanSquareError.html\">MeanSquareError</a>","id":"module:analytics~metrics.MeanSquareError","children":[]},{"label":"<a href=\"module-analytics-metrics.PredictionCurve.html\">PredictionCurve</a>","id":"module:analytics~metrics.PredictionCurve","children":[]},{"label":"<a href=\"module-analytics-metrics.R2Score.html\">R2Score</a>","id":"module:analytics~metrics.R2Score","children":[]},{"label":"<a href=\"module-analytics-metrics.RootMeanSquareError.html\">RootMeanSquareError</a>","id":"module:analytics~metrics.RootMeanSquareError","children":[]}]}]},{"label":"<a href=\"module-datasets.html\">datasets</a>","id":"module:datasets","children":[]},{"label":"<a href=\"module-fs.html\">fs</a>","id":"module:fs","children":[{"label":"<a href=\"module-fs.FIn.html\">FIn</a>","id":"module:fs.FIn","children":[]},{"label":"<a href=\"module-fs.FOut.html\">FOut</a>","id":"module:fs.FOut","children":[]}]},{"label":"<a href=\"module-ht.html\">ht</a>","id":"module:ht","children":[{"label":"<a href=\"module-ht.IntFltMap.html\">IntFltMap</a>","id":"module:ht.IntFltMap","children":[]},{"label":"<a href=\"module-ht.IntIntMap.html\">IntIntMap</a>","id":"module:ht.IntIntMap","children":[]},{"label":"<a href=\"module-ht.IntStrMap.html\">IntStrMap</a>","id":"module:ht.IntStrMap","children":[]},{"label":"<a href=\"module-ht.StrFltMap.html\">StrFltMap</a>","id":"module:ht.StrFltMap","children":[]},{"label":"<a href=\"module-ht.StrIntMap.html\">StrIntMap</a>","id":"module:ht.StrIntMap","children":[]},{"label":"<a href=\"module-ht.StrStrMap.html\">StrStrMap</a>","id":"module:ht.StrStrMap","children":[]}]},{"label":"<a href=\"module-la.html\">la</a>","id":"module:la","children":[{"label":"<a href=\"module-la.BoolVector.html\">BoolVector</a>","id":"module:la.BoolVector","children":[]},{"label":"<a href=\"module-la.IntVector.html\">IntVector</a>","id":"module:la.IntVector","children":[]},{"label":"<a href=\"module-la.Matrix.html\">Matrix</a>","id":"module:la.Matrix","children":[]},{"label":"<a href=\"module-la.SparseMatrix.html\">SparseMatrix</a>","id":"module:la.SparseMatrix","children":[]},{"label":"<a href=\"module-la.SparseVector.html\">SparseVector</a>","id":"module:la.SparseVector","children":[]},{"label":"<a href=\"module-la.StrVector.html\">StrVector</a>","id":"module:la.StrVector","children":[]},{"label":"<a href=\"module-la.Vector.html\">Vector</a>","id":"module:la.Vector","children":[]}]},{"label":"<a href=\"module-qm.html\">qm</a>","id":"module:qm","children":[{"label":"<a href=\"module-qm.Base.html\">Base</a>","id":"module:qm.Base","children":[]},{"label":"<a href=\"module-qm.CircularRecordBuffer.html\">CircularRecordBuffer</a>","id":"module:qm.CircularRecordBuffer","children":[]},{"label":"<a href=\"module-qm.FeatureSpace.html\">FeatureSpace</a>","id":"module:qm.FeatureSpace","children":[]},{"label":"<a href=\"module-qm.Iterator.html\">Iterator</a>","id":"module:qm.Iterator","children":[]},{"label":"<a href=\"module-qm.Record.html\">Record</a>","id":"module:qm.Record","children":[]},{"label":"<a href=\"module-qm.RecordSet.html\">RecordSet</a>","id":"module:qm.RecordSet","children":[]},{"label":"<a href=\"module-qm.Store.html\">Store</a>","id":"module:qm.Store","children":[]},{"label":"<a href=\"module-qm.StreamAggr.html\">StreamAggr</a>","id":"module:qm.StreamAggr","children":[]}]},{"label":"<a href=\"module-statistics.html\">statistics</a>","id":"module:statistics","children":[]}],
        openedIcon: ' &#x21e3;',
        saveState: true,
        useContextMenu: false
    });

    // add event handlers
    // TODO
})(jQuery);
