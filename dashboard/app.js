/* SPDX-License-Identifier: GPL-2.0-or-later */
(function () {
    var data = window.SNAPSHOT;

    function countsText(counts) {
        if (!counts) {
            return "";
        }
        return counts.works + " works, " + counts.partial + " partial, " +
            counts.broken + " broken, " + counts.missing + " missing, " +
            counts.untested + " untested";
    }

    function percentText(value) {
        if (value === null || value === undefined) {
            return "n/a";
        }
        return value + "%";
    }

    function renderIndex() {
        var root = document.getElementById("app");
        var headline = data.headline || {};
        var html = "<h1>Windows 10 22H2 compatibility</h1>" +
            "<p class='sub'>Build " + data.build.version + " · " + data.build.git + "</p>" +
            "<div class='cards'>" +
            "<div class='card'><span>Driver ABI</span><b>" + percentText(headline.percent) + "</b></div>" +
            "<div class='card'><span>Overall</span><b>" + percentText(data.overall.percent) + "</b></div>" +
            "<div class='card'><span>Boot</span><b>" + data.boot.status + "</b></div>" +
            "</div>" +
            "<p class='note'>Weights: driver ABI 40, kernel exports 25, syscalls 15, drivers 10, graphics 5, applications 5. A category with only untested rows is left out and the rest are scaled to 100. Syscall number match is " +
            data.numberMatch + " because the reference list is names only.</p>" +
            "<table><thead><tr><th>Category</th><th>Percent</th><th>Counts</th></tr></thead><tbody>";
        data.categories.forEach(function (cat) {
            html += "<tr class='link' data-href='category.html?id=" + cat.id + "'><td>" + cat.title +
                "</td><td>" + percentText(cat.percent) + "</td><td>" + countsText(cat.counts) + "</td></tr>";
        });
        html += "</tbody></table>";
        root.innerHTML = html;
    }

    function renderCategory() {
        var id = new URLSearchParams(location.search).get("id") || "driverAbi";
        var cat = null;
        data.categories.forEach(function (item) {
            if (item.id === id) {
                cat = item;
            }
        });
        var root = document.getElementById("app");
        var title = cat ? cat.title : id;
        var html = "<p><a href='index.html'>Overview</a></p><h1>" + title + "</h1>" +
            "<p class='sub'>" + percentText(cat && cat.percent) + " · " + countsText(cat && cat.counts) + "</p>";
        if (id === "syscalls") {
            html += "<section><h2>Top 10</h2><table><thead><tr><th>Syscall</th><th>Status</th><th>Number match</th></tr></thead><tbody>";
            data.topSyscalls.forEach(function (name) {
                var row = null;
                data.items.forEach(function (item) {
                    if (item.category === "syscalls" && item.name === name) {
                        row = item;
                    }
                });
                html += "<tr><td>" + name + "</td><td class='" + (row ? row.status : "") + "'>" +
                    (row ? row.status : "missing") + "</td><td>" + (row ? row.numberMatch : "untested") + "</td></tr>";
            });
            html += "</tbody></table></section>";
        }
        html += "<section><h2>All items</h2><table><thead><tr><th>Name</th><th>Group</th><th>Status</th><th>Evidence</th><th>Notes</th></tr></thead><tbody>";
        data.items.forEach(function (item) {
            if (item.category !== id) {
                return;
            }
            html += "<tr><td>" + item.name + "</td><td>" + (item.group || "") + "</td><td class='" + item.status + "'>" +
                item.status + "</td><td>" + (item.evidence || "") + "</td><td>" + (item.notes || "") + "</td></tr>";
        });
        html += "</tbody></table></section>";
        root.innerHTML = html;
    }

    document.addEventListener("click", function (event) {
        var row = event.target.closest("tr.link");
        if (row && row.dataset.href) {
            location.href = row.dataset.href;
        }
    });

    if (document.body.dataset.page === "category") {
        renderCategory();
    }
    else {
        renderIndex();
    }
}());
