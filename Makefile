cp:
	cp doc/html/* api
	cp doc/man/*.html man
	cp doc/manual/ach-manual.html manual/index.html
	cp -r doc/javadoc .


sync:
	rsync --progress --recursive         \
	      --exclude 'doc'                \
	      --include 'man'                \
	      --include 'manual'             \
	      --include 'api'                \
	      --include 'images'             \
	      --include 'javadoc'            \
	      --include 'javadoc/org'        \
	      --include 'javadoc/org/golems' \
	      --include 'javadoc/org/golems/ach' \
	      --include 'javadoc/resources'  \
	      --include '*.html'             \
	      --include '*.css'              \
	      --include '*.png'              \
	      --include '*.gif'              \
	      --exclude '*'                  \
	      ./ code.golems.org:/srv/www/code.golems.org/pkg/ach
