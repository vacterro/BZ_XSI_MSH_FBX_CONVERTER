(
	--	XSI Import Rollout 
	--  Version:  1.030 Beta developed by Michael Lawler (archangel35757@yahoo.com)
	--			  This version is derived from v1.013 (created by Tsvetan Stoyanov) and
	--			  contains extensive modifications for Star Trek:Voyager-Elite Force
	--	Compatible with 3ds Max (this version breaks compatibility with Gmax)
	--	Supports dotXSI file format versions: 1.1 & 1.3
	
	-- Global Variables
	xsiDataCA = attributes xsiData
	(
		parameters xsiParams
		(
			tmBasepose	type:#matrix3		default:(matrix3(1))		
			tmSRT		type:#matrix3Tab	tabSizeVariable:true
			keysPos		type:#point3Tab		tabSizeVariable:true 
			keysRot		type:#point3Tab		tabSizeVariable:true
			keysScale	type:#point3Tab		tabSizeVariable:true 
			
			--parameter block event handlers
			/* none */
			
		)		
	)
	
	inFile
	lineOut = #()
	xsiFileList=#()
	
	-- Local Variables
	local hierarchyRoot
	local xsiFPS = 20
	local xsiAngleUnits
	local startFrame = 1
	local numFrames
	local Directory
	local bIsMeshObj = false
	local bFoundFrame = false
	local tmX90 = matrix3 [1,0,0] [0,0,1] [0,-1,0] [0,0,0] 
	local tmX90Inv = inverse tmX90
	local crrObj
	local import_rollout_version = #("1", "030")
	local objects_arr = #()
	local file_templates = ""
	local bNormals = true
	local bMeshTextureCoords = true
	local bMeshMaterial = true
	local bObjectAnimation = true
	local bAnimImport = true
	local bSkinEnvelope = true
	local max_filepos = 0
	local Main_rll
	local bProgressFlag = false
	
	local axisorder = #(
		#(1,2,3), #(1,2,6), #(1,3,2), #(1,3,5), #(1,5,3), #(1,5,6),
		#(1,6,2), #(1,6,5), #(2,1,3), #(2,1,6), #(2,3,1), #(2,3,4),
		#(2,4,3), #(2,4,6), #(2,6,1), #(2,6,4), #(3,1,2), #(3,1,5),
		#(3,2,1), #(3,2,4), #(3,4,2), #(3,4,5), #(3,5,1), #(3,5,4),
		#(4,2,3), #(4,2,6), #(4,3,2), #(4,3,5), #(4,5,3), #(4,5,6),
		#(4,6,2), #(4,6,5), #(5,1,3), #(5,1,6), #(5,3,1), #(5,3,4),
		#(5,4,3), #(5,4,6), #(5,6,1), #(5,6,4), #(6,1,2), #(6,1,5),
		#(6,2,1), #(6,2,4), #(6,4,2), #(6,4,5), #(6,5,1), #(6,5,4)
	)
	
	local axisorder_id = 11
	local grandpappy = #()
	local bPrerotateRoot = false
	local ErrorLog = ""
	local bDeletePrefix = false

	-- Functions
	fn PrepareToRead lne = (
		lineIn = #()
		lineOut = #()
		insertItem lne[1] lineOut 1
		major_version = (substring lne[2] 1 2) as integer
		minor_version = (substring lne[2] 3 2) as integer
		insertItem (major_version as string) lineOut 2
		insertItem (minor_version as string) lineOut 3
		type = substring lne[2] 5 3
		insertItem type lineOut 4
		file_pos = filePos inFile;
		insertItem file_pos lineOut 5 --file position after header line
		endOfFile = false
		while endOfFile == false do (
			lineIn = readLine inFile
			endOfFile = eof inFile
			lineIn = filterString lineIn ", -;}\t"
			in_line_word_test = (
				if lineIn[1] != undefined then (
					lineIn[1] as integer
				)
				else (
					0
				)
			)
			case in_line_word_test of (
				(0):()
				(undefined):(
					isInserted = findString file_templates lineIn[1]
					if isInserted == undefined do (
						file_templates = file_templates + lineIn[1] + "\n"
					)
				)
			)
		)
	)
	
	fn ValidateDirectoryName dir = (
	  f = 0;
	  lineIn = dir;
	  while f != undefined do (
		  f = findString lineIn "\t"
		  lineIn = try(replace lineIn f 2 "\\t") catch(lineIn)
	  )
	  f = 0;
	  while f != undefined do (
		  f = findString lineIn "\n"
		  lineIn = try(replace lineIn f 2 "\\n") catch(lineIn)
	  )
	  f = 0;
	  while f != undefined do (
		  f = findString lineIn "\r"
		  lineIn = try(replace lineIn f 2 "\\r") catch(lineIn)
	  )
	  lineIn
	)
	
	fn Get_SI_CoordinateSystem o = (
	  readDelimitedString inFile "}"
	)
	
	-- The function Get_Frame
	-- takes the second string in the 'inStrings' array and creates the new empty mesh of the object with the given name.
	-- inserts the object into the objects array.
	-- verifies if there is a previous member in the objects array available and sets it as parent to the object.
	fn Get_Frame inStrings = (
		bIsMeshObj = false
		bFoundFrame = false
		--get the minor version of incoming dotXSI file
		arrayElement = case lineOut[3] of (
			("1"):(2)
			("3"):(2)
		)
		objName = inStrings[arrayElement]
		/*
			Here we need to read ahead and see if this frame template contains
			a child "Mesh" template.  If it does then create the object as an 
			Editable Mesh, otherwise create it as a Point Helper object.
		*/
		--store current file position
		tempFilePos = filePos inFile
		--read ahead and look for "Mesh" or "Frame"
		do (
			tempLine = readLine inFile
			tempLine = filterString tempLine ", ;\t"
			if (tempLine[1] == "Mesh") do ( exit with bIsMeshObj = true )
			if (tempLine[1] == "Frame") do ( exit with bFoundFrame = true )
			endOfStream = eof inFile
		)
		while ( ( bIsMeshObj != true or bFoundFrame != true ) and not endOfStream == true )
		
		--done with searching, reset the file position back to previous location
		seek inFile tempFilePos
		--is it a mesh object?
		if bIsMeshObj then ( 
			crrObj = editable_mesh name:objName 
			--add custom attribute to store xsi data on the node itself
			custAttributes.add crrObj xsiDataCA BaseObject:true
		) 
		else (
			crrObj = point	name:objName size:1 axisLength:1 box:true wirecolor:green \
							cross:false axistripod:false centermarker:false 
			custAttributes.add crrObj xsiDataCA BaseObject:true
		)
		insertItem crrObj objects_arr (objects_arr.count + 1)
		try (
			crrObj.parent = objects_arr[objects_arr.count - 1]
		) 
		catch (
			--Must be a top-level object
			crrObj.parent = undefined
		)
	)
	
	fn Get_SI_Envelope = (
		if bSkinEnvelope == true do (
			geometry_obj = filterString (readLine inFile) " \t\";"
			geometry_obj = getNodeByName geometry_obj[1]
			bone_obj = filterString (readLine inFile) " \t\";"
			bone_obj = getNodeByName bone_obj[1]
			numberOfWeighedVertices = readValue inFile
			
			if numberOfWeighedVertices == 0 then (
				return (readDelimitedString inFile "}")
			)
			
			a = try(geometry_obj.skin) catch(addmodifier geometry_obj (skin());geometry_obj.skin;)
			max modify mode
			select geometry_obj
			boneID = undefined
			numberOfBones = skinops.getnumberbones a
			temp_numberOfBones = case numberOfBones of (
				(0):(1)
				default:(numberOfBones)
			)
			
			absentFlag = true
			
			for i = 1 to temp_NumberOfBones do (
				bonename = try(skinops.getbonename a i 0) catch()
				if bonename == bone_obj.name do (
					boneID = i;
					absentFlag = undefined;
					exit;
				)
			)
			
			if absentFlag == true do (
				skinops.addbone a bone_obj 0
				select geometry_obj
				boneID = numberOfBones + 1
			)
			
			for i = 1 to numberOfWeighedVertices do (
				vertexID = readValue inFile
				weight = readValue inFile
				skipToNextLine inFile
				skinops.setvertexweights a (vertexID + 1) boneID (weight/100.0)
			)
		)
		
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_EnvelopeList = (
		lineIn = #()
		while lineIn[1] != "}" do (
			lineIn = readLine inFile
			lineIn = filterString lineIn ", ;\t";
			
			case lineIn[1] of (
				("SI_Envelope"):(Get_SI_Envelope())
				("}"):()
				default: ()
			)
		)
	)
	
	fn Get_SI_Angle = (
		--specifies angle units in xsi file: 0 = degrees; 1 = radians
		xsiAngleUnits = (readValue inFile) as integer
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_Camera o = (
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_Light o = (
		readDelimitedString inFile "}"
	)
	fn Get_SI_Ambience = (
		R = readValue inFile; G = readValue inFile; B = readValue inFile;
		ambientColor = color R G B
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_Fog = (
		readDelimitedString inFile "}"
	)
	
	fn Get_FrameTransformMatrix o = (
		if axisorder_id == 1 then 
		(
			x_axis_x = readValue inFile; y_axis_x = readValue inFile; z_axis_x = readValue inFile; readValue inFile;
			x_axis_y = readValue inFile; y_axis_y = readValue inFile; z_axis_y = readValue inFile; readValue inFile;
			x_axis_z = readValue inFile; y_axis_z = readValue inFile; z_axis_z = readValue inFile; readValue inFile;
			x_axis_pos = readValue inFile; y_axis_pos = readValue inFile; z_axis_pos = readValue inFile; readValue inFile;
		)
		else (
			y_axis_y = readValue inFile; y_axis_z = readValue inFile; y_axis_x = readValue inFile; readValue inFile;
			z_axis_y = readValue inFile; z_axis_z = readValue inFile; z_axis_x = readValue inFile; readValue inFile;
			x_axis_y = readValue inFile; x_axis_z = readValue inFile; x_axis_x = readValue inFile; readValue inFile;
			y_axis_pos = readValue inFile; z_axis_pos = readValue inFile; x_axis_pos = readValue inFile;readValue inFile;
		)
		
		m = try (o.parent.transform) catch ( matrix3(1) )
		m.row4 = [0.0,0.0,0.0]
		row3 = #(x_axis_y, x_axis_z, x_axis_x, -x_axis_y, -x_axis_z, -x_axis_x)
		row1 = #(y_axis_y, y_axis_z, y_axis_x, -y_axis_y, -y_axis_z, -y_axis_x)
		row2 = #(z_axis_y, z_axis_z, z_axis_x, -z_axis_y, -z_axis_z, -z_axis_x)
		
		xo = amax #((findItem axisorder[axisorder_id] 1),(findItem axisorder[axisorder_id] 4))
		yo = amax #((findItem axisorder[axisorder_id] 2),(findItem axisorder[axisorder_id] 5))
		zo = amax #((findItem axisorder[axisorder_id] 3),(findItem axisorder[axisorder_id] 6))
		

		if axisorder_id == 1 then 
		(
			row1 = [ x_axis_x, y_axis_x, z_axis_x ]
			row2 = [ x_axis_y, y_axis_y, z_axis_y ]
			row3 = [ x_axis_z, y_axis_z, z_axis_z ]
		)
		else (
			row1 = [row1[xo],row1[yo],row1[zo]]*m
			row2 = [row2[xo],row2[yo],row2[zo]]*m
			row3 = [row3[xo],row3[yo],row3[zo]]*m
		)
		
		rows = #(row1, row2, row3, -row1, -row2, -row3)
		rao = #(rows[xo],rows[yo],rows[zo])
		m.row4 = try (o.parent.transform.row4) catch ([0.0,0.0,0.0])
		row4 = #(y_axis_pos, z_axis_pos, x_axis_pos, -y_axis_pos, -z_axis_pos, -x_axis_pos)
		row4 = [row4[xo],row4[yo],row4[zo]]*m
		
		if axisorder_id == 1 then 
		(
			row4 = [ x_axis_pos, y_axis_pos, z_axis_pos ]
			matrix = matrix3 row1 row2 row3 row4		--matrix is in XSI coordinate system
			matrix = ( tmX90Inv * matrix * tmX90 ) * m	--converts matrix to 3dsMax Coordinate System
		)
		else (
			matrix = matrix3 rao[1] rao[2] rao[3] row4
		)
		
		o.transform = matrix

		If grandpappy.count != 1 do (
			grandpappy[1] = o
			rotate o (eulerangles Main_rll.spn_RootRotateX.value 0 0)
			rotate o (eulerangles 0 Main_rll.spn_RootRotateY.value 0)
			rotate o (eulerangles 0 0 Main_rll.spn_RootRotateZ.value)
		)
		
		readDelimitedString inFile "}"
		--return the matrix transform value
		matrix
	)
	
	fn Get_SI_FrameBasePoseMatrix o = (
		o.tmBasepose = ( Get_FrameTransformMatrix o )
		at time 0 o.transform = o.tmBasepose
		--return the matrix transform value
		o.tmBasepose
	)
	
	fn Get_SI_MeshVertexColors o = (
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_MeshNormals o = (
		if bNormals == true do (
			try (
				max modify mode
				select o
				addModifier o (Edit_Normals())
				
				faceVertices = #()
				for f = 1 to (o.numfaces) do (
					vertArray = #()
					verts = getface o f --gets the 3 vertices of the face as a point3 value
					vertArray = #(verts.x as integer,verts.y as integer,verts.z as integer) --recast as array
					faceVertices[f] = vertArray
				)

				vertNormal_arr = #()
				numVertexNormals = readValue inFile
				xo = amax #((findItem axisorder[axisorder_id] 1),(findItem axisorder[axisorder_id] 4))
				yo = amax #((findItem axisorder[axisorder_id] 2),(findItem axisorder[axisorder_id] 5))
				zo = amax #((findItem axisorder[axisorder_id] 3),(findItem axisorder[axisorder_id] 6))
				for f = 1 to numVertexNormals do (
					axis2 = #(readValue inFile,readValue inFile,readValue inFile);
					if axisorder_id == 1 then (
					norm = ( tmX90Inv * transMatrix [ axis2[1], axis2[2], axis2[3] ] * tmX90 ).row4 -- * o.transform
					)
					else (
					norm = [axis2[xo],axis2[yo],axis2[zo]]*(o.transform)
					)
					skipToNextLine inFile
					vertNormal_arr[f] = norm
				)
				
				numFaceNormals = readValue inFile
				iter = 0
				for f = 1 to numFaceNormals do (
					faceIN = (readValue inFile) + 1
					vertIndices = faceVertices[f]
					numVertNormalsPerFace = readValue inFile
					if numVertexNormals != $.Edit_Normals.GetNumNormals() do (
					print "Number of vertex normals mismatch!"
					)
					for v = 1 to numVertNormalsPerFace do (
						norm = vertNormal_arr[readValue inFile + 1]
						iter += 1
						--vertIndices[v] yields the actual vertexID in the face
						$.Edit_Normals.SetNormalExplicit iter explicit:true
						in coordsys local $.Edit_Normals.SetNormal iter vertNormal_arr[iter]
					)
					skipToNextLine inFile
				)
				deselect o
			)
			catch (
				myString = "< " + o as string + " >" + "Mesh bNormals"
				ErrorLog = ErrorLog + myString + "\n"
			)
		)
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_MeshTextureCoords o = (
		if bMeshTextureCoords == true do (
			try (
				/*
				if classOf o != Editable_Mesh do (
					--convertToMesh o
				)
				*/
				NumTVerts = readValue inFile
				setNumTVerts o NumTVerts
				buildTVFaces o
				for v = 1 to NumTVerts do (
					UVCoordinates = [readValue inFile,readValue inFile,0]
					setTVert o v UVCoordinates
					skipToNextLine inFile
				)
				numTVFaces = readValue inFile
				for f = 1 to numTVFaces do (
					TVFaceIN = readValue inFile
					numTVerts = readValue inFile
					TVFace_vert_indices = #()
					for v = 1 to numTVerts do (
						TVExpr = (readValue inFile) + 1;
						TVFace_vert_indices[v] = TVExpr;
					)
					skipToNextLine inFile
					setTVFace o (TVFaceIN + 1) TVFace_vert_indices[1] TVFace_vert_indices[2] TVFace_vert_indices[3]
				)
			)
			catch (
				myError = "< "+o as string+" >"+"Mesh Texture Coordinates"
				ErrorLog = ErrorLog + myError + "\n"
			)
		)
		readDelimitedString inFile "}"
	)
		
	fn Get_TextureFilename o m = (
		if bMeshMaterial == true do (
			lineIn = filterString (readLine inFile) "\t\";"
			path = ((getFilenamePath Directory) + "\\" + lineIn[1])
			tmap = bitmaptexture()
			tmap.filename = path
			o.Material[m].maps[2] = tmap
		)
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_Texture2D o m = (
		if bMeshMaterial == true do (
			tmap = bitmaptexture()
			directory_filename = readLine inFile
			directory_filename = filterString directory_filename "\";\t"
			directory_filename = ((getFilenamePath Directory)+"\\"+directory_filename[1])
			tmap.filename = directory_filename
			o.Material[m].maps[2] = tmap
			readValue inFile
			try (o.Material[m].maps[2].bitmap.width = readValue inFile) catch ()
			try (o.Material[m].maps[2].bitmap.height = readValue inFile) catch ()
		)
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_Material o m = (
		if bMeshMaterial == true do (
			try (
				o.Material.MaterialList[m].diffuse.r = (readValue inFile)*255
				o.Material.MaterialList[m].diffuse.g = (readValue inFile)*255
				o.Material.MaterialList[m].diffuse.b = (readValue inFile)*255
				o.Material.MaterialList[m].diffuse.a = (readValue inFile)*255
				skipToNextLine inFile
				o.Material.MaterialList[m].specularLevel = (readValue inFile)*100
				o.Material.MaterialList[m].specular.r = (readValue inFile)*255
				o.Material.MaterialList[m].specular.g = (readValue inFile)*255
				o.Material.MaterialList[m].specular.b = (readValue inFile)*255
				skipToNextLine inFile
				readValue inFile
				readValue inFile
				readValue inFile
				skipToNextLine inFile
				readValue inFile
				o.Material.MaterialList[m].ambient.r = (readValue inFile)*255
				o.Material.MaterialList[m].ambient.g = (readValue inFile)*255
				o.Material.MaterialList[m].ambient.b = (readValue inFile)*255
				skipToNextLine inFile
			)
			catch (
				myError = "< " + o as string + " >" + " Mesh Material"
				ErrorLog = ErrorLog+myError + "\n"
			)
		)
		in_line3 = #()
		while in_line3[1] != "}" do (
			in_line3 = readLine inFile
			in_line3 = filterString in_line3 ", -;\t"
			case in_line3[1] of (
				("SI_Texture2D"):(Get_SI_Texture2D o m)
				("TextureFilename"):(Get_TextureFilename o m)
				("}"):()
				default:()
			)
		)
	)
	
	fn Get_MeshMaterialList o = (
		if bMeshMaterial == true do (
			try (
				/*
				if classOf o != Editable_Mesh do (
					--convertToMesh o
				)
				*/
				Matcount = readValue inFile
				num_faces = readValue inFile
				multiMat = multimaterial numsubs:Matcount
				o.material = multiMat
				for f = 1 to num_faces do (
					setFaceMatID O f ((readValue inFile) + 1)
				)
			)
			catch (
				myError = "< "+o as string+" >"+" Mesh Material List"+"\n"
				ErrorLog = ErrorLog+myError+"\n"
			)
		)
		in_line2 = #()
		matCounter = 1
		while in_line2[1] != "}" do (
			in_line2 = readLine inFile
			in_line2 = filterString in_line2 ", -;\t"
			case in_line2[1] of (
				("SI_Material"):(Get_SI_Material o (matCounter); matCounter = matCounter + 1;)
				("}"):()
				default:()
			)
		)
	)
	
	fn Get_Mesh o = (
		--filter out Point Helper objects
		if superClassOf o == GeometryClass do (
			--convertTo o editable_poly
			numberOfVerticies = readValue inFile
			/*
			for i = 1 to numberOfVerticies do (
				polyop.createVert o [0.0,0.0,0.0]
			)
			*/
			meshop.setNumVerts o numberOfVerticies --new vertices are created at [0,0,0] local space			
			for i = 1 to numberOfVerticies do (
				if axisorder_id == 1 then (
					vert_x = readValue inFile; vert_y = readValue inFile; vert_z = readValue inFile;
				)
				else (
					vert_y = readValue inFile; vert_z = readValue inFile; vert_x = readValue inFile;
				)
				vert_pos = #(vert_y,vert_z,vert_x,-vert_y,-vert_z,-vert_x)
				xo = amax #((findItem (axisorder[axisOrder_ID]) 1),(findItem axisorder[axisOrder_ID] 4))
				yo = amax #((findItem (axisorder[axisOrder_ID]) 2),(findItem axisorder[axisOrder_ID] 5))
				zo = amax #((findItem axisorder[axisOrder_ID] 3),(findItem axisorder[axisOrder_ID] 6))
				vert_pos_axisorder = [vert_pos[xo],vert_pos[yo],vert_pos[zo]]
				
				/*
				Here we do a coordinate transformation to get vertex position in 3dsMax coordinates
				*/
				if axisorder_id == 1 then (
					vertPos = [vert_x, vert_y, vert_z] --this is in XSI coordinates
					tempTransMat = ( tmX90Inv * ( transMatrix vertPos ) * tmX90 ) * o.transform
					vert_pos_axisorder = tempTransMat.row4 --now the vertex is in 3dsMax coordinates
				)
				else (
					vert_pos_axisorder = vert_pos_axisorder*o.transform
				)
				skipToNextLine inFile
				--polyop.setVert o #(i) vert_pos_axisorder
				meshop.setVert o #(i) vert_pos_axisorder
			)
			
			numberOfFaces = readValue inFile
			for f = 1 to numberOfFaces do (
				numberOfFaceVerticies = readValue inFile
				verticies_array = #()
				for v = 1 to numberOfFaceVerticies do (
					insertItem ((readValue inFile) + 1) verticies_array v
				)
				--polyop.createPolygon o verticies_array
				meshop.createPolygon o verticies_array
				skipToNextLine inFile
			)
			update o
			
			local in_line1 = #()
			while in_line1[1] != "}" do (
				in_line1 = readLine inFile
				in_line1 = filterString in_line1 ", -;\t"
				case in_line1[1] of (
					("SI_MeshNormals"):(Get_SI_MeshNormals o)
					("SI_MeshVertexColors"):(Get_SI_MeshVertexColors o)
					("SI_MeshTextureCoords"):(Get_SI_MeshTextureCoords o)
					("MeshMaterialList"):(Get_MeshMaterialList o)
					("}"):()
					default:()
				)
			)
		)
	)
	
	fn Get_SI_AnimationKey o = (
		if bObjectAnimation == true do (
			xo = amax #((findItem axisorder[axisorder_id] 1),(findItem axisorder[axisorder_id] 4))
			yo = amax #((findItem axisorder[axisorder_id] 2),(findItem axisorder[axisorder_id] 5))
			zo = amax #((findItem axisorder[axisorder_id] 3),(findItem axisorder[axisorder_id] 6))
			keyType = readValue inFile
			numFrames = readValue inFile
			local tqp = (quat 0 0 0 1)
				
			for i = 1 to numFrames do (
				if axisorder_id == 1 then (
					--adding 1 to make animations start on frame 1 (frame 0 reserved for Basepose)
					frameNum = 1 + (readValue inFile) 
				)
				else (
					frameNum = readValue inFile
				)
				expr = readValue inFile --this value means the number of values that follows it.
				expr_arr = #(); expr_arr.count = expr + expr;

				for j = 1 to expr do (
					expr_arr[j] = readValue inFile;--print expr_arr[j]
					expr_arr[j+expr] = -expr_arr[j]
				)
				
				case keyType of (
					(0):( --quaternion rotation
						--Softimage quaternion notation format: (W, X, Y, Z)
						--3dsMax Maxscript quaternion notation format: quat <x_float> <y_float> <z_float> <w_float>
						--ST:Voyager-EF does not use Quaternions... so I'm not touching this.
						temp_quat = quat -expr_arr[1] -expr_arr[3] expr_arr[2] expr_arr[4]
						temp_quat = temp_quat + (quat 1 0 0 0)
						quatAsEulerAngle = quatToEuler temp_quat
						eulerAsAngleAxis = quatAsEulerAngle as angleAxis
						rc = o.rotation.controller
						addNewKey rc frameNum
						try (
							rc.x_rotation.keys[i].value = quatAsEulerAngle.x; rc.y_rotation.keys[i].value = quatAsEulerAngle.y; rc.z_rotation.keys[i].value = quatAsEulerAngle.z
						)
						catch (
							tq = quat (temp_quat.x*(/*-*/1)) temp_quat.y temp_quat.z temp_quat.w
							rc.keys[i].value = tqp - (tq)/*- tqp*/
							tqp = tq
						)
					)
					(1):( --scale
						if axisorder_id == 1 then (
							--store values to customAttribute
							o.keysScale[i] = [ expr_arr[1], expr_arr[2], expr_arr[3] ]
						)
						else (
							sc = o.scale.controller
							addNewKey sc frameNum
							sc.keys[i].value.x = expr_arr[3]; sc.keys[i].value.y = expr_arr[1]; sc.keys[i].value.z = expr_arr[2];
						)
					)
					(2):( --translation
						if axisorder_id == 1 then (
							--store values to customAttribute
							o.keysPos[i] = [ expr_arr[1], expr_arr[2], expr_arr[3] ]
							--xsiX = expr_arr[1]
							--xsiY = expr_arr[2]
							--xsiZ = expr_arr[3]
							--transform to 3ds Max coordinates
							--pos3dsMax = ( tmX90Inv * ( transMatrix [xsiX,xsiY,xsiZ] ) * tmX90 ).row4
						)
						else (
							tc = o.position.controller
							addNewKey tc frameNum
							try (
								tc.x_position.keys[i].value = expr_arr[xo];
								tc.y_position.keys[i].value = expr_arr[yo];
								tc.z_position.keys[i].value = expr_arr[zo];
							)
							catch (
								tc.keys[i].value = [expr_arr[xo],expr_arr[yo],expr_arr[zo]]
							)
						)
					)
					(3):( --Euler rotation
						if axisorder_id == 1 then (
						
							if xsiAngleUnits == 1 then ( 
								--angles are in radians. Convert to degrees.
								o.keysRot[i] = [ radToDeg(expr_arr[1]), radToDeg(expr_arr[2]), radToDeg(expr_arr[3]) ]
							)
							else (
								o.keysRot[i] = [ expr_arr[1], expr_arr[2], expr_arr[3] ]
							)
							/*
							xsiXrot = expr_arr[1]
							xsiYrot = expr_arr[2]
							xsiZrot = expr_arr[3]
							--convert XSI Euler angles to 3dsMax Quaternion
							xsiAngles2Quat = (eulerAngles xsiXrot xsiYrot xsiZrot) as quat
							_3dsMaxQuat = ( transform (xsiAngles2Quat) tmX90 ) * (-1)
							--set controller value
							at time frameNum rc.value = _3dsMaxQuat
							*/
						)
						else (
							rc = o.rotation.controller
							addNewKey rc frameNum
							
							if xsiAngleUnits == 1 do ( 
								--angles are in radians. Convert to degrees.
								expr_arr[xo] = radToDeg( expr_arr[xo] )
								expr_arr[yo] = radToDeg( expr_arr[yo] )
								expr_arr[zo] = radToDeg( expr_arr[zo] )
							)
							
							euler = eulerAngles expr_arr[xo] expr_arr[yo] expr_arr[zo]
							
							try (
								rc.x_rotation.keys[i].value = expr_arr[(xo)]
								rc.y_rotation.keys[i].value = expr_arr[(yo)]
								rc.z_rotation.keys[i].value = expr_arr[(zo)]
							)
							catch (
								rc.keys[i].value = tqp - (euler as quat)
								tqp = euler as quat
							)
						)
					)
				)
				skipToNextLine inFile
			)
		)
		readDelimitedString inFile "}"
	)
	
	fn Get_SI_AnimationParamKey o = (
		readDelimitedString inFile "}"
		readDelimitedString inFile "}"
	)
	
	fn Get_Animation = (
		if bObjectAnimation == true do (
			--readDelimitedString inFile "}"
		)
		lineIn = #()
		local temp_obj
		while lineIn[1] != "}" do (
			lineIn = readLine inFile
			lineIn = filterString lineIn ", ;\t";
			case lineIn[1] of (
				(undefined):()
				("SI_AnimationKey"):(Get_SI_AnimationKey temp_obj)
				("SI_AnimationParamKey"):(Get_SI_AnimationParamKey 0)
				("}"):()
				default:(
					if (lineIn[1])[1] == "{" then (
						temp_line = filterString lineIn[1] "{ }\t"
						temp_obj = try (getNodeByName (temp_line[1])) catch (myError = "< "+temp_line[1]+" >"+" Animation";ErrorLog += myError+"\n")
					)
					else (
					)
				)
			)
		)
	)
	
	fn Get_AnimationSet o = (
		lineIn = #()
		while lineIn[1] != "}" do (
			lineIn = readLine inFile
			lineIn = filterString lineIn ", -;\t"
			case lineIn[1] of (
				("Animation"):(Get_Animation())
				("}"):()
				default:()
			)
		)
	)
	
	fn UpdateObject o = (
		if o != undefined and not isDeleted o do (
			if classOf o == Editable_Mesh then (
				update o
			)
			else (
			)
		)
	)
	
	fn SetAnimation o frameTime = (
		if classOf o == Point and numFrames != undefined do ( --only write animation data for Point Helpers (i.e., bones)
			with animate on
			( 
				at time frameTime 
				(
					--key position
					if o.keysPos[frameTime] != undefined do (
						o.pos.controller.value = (tmX90Inv * (transMatrix o.keysPos[frameTime]) * tmX90).row4
					)
					--key scale
					if o.keysScale[frameTime] != undefined do (
						o.scale.controller.value = o.keysScale[frameTime]
					)
					--key rotation
					--convert XSI Euler angles to 3dsMax Quaternion
					if o.keysRot[frameTime] != undefined do (
						xsiXrot = o.keysRot[frameTime].x
						xsiYrot = o.keysRot[frameTime].y
						xsiZrot = o.keysRot[frameTime].z
						xsiAngles2Quat = (eulerAngles xsiXrot xsiYrot xsiZrot) as quat
						_3dsMaxQuat = ( transform (xsiAngles2Quat) tmX90 ) * (-1)
						--set controller value
						o.rotation.controller.value = _3dsMaxQuat
						--store the SRT transform at frameTime 
						  --(note: tmSRT is not saved for time 0 in order to keep array index matching frame time)
						o.tmSRT[frameTime] = (scaleMatrix o.scale.controller.value)*(_3dsMaxQuat as matrix3)*(transMatrix o.pos.controller.value)
					)
				)
			)
			if o.children.count > 0 do (
				for i = 1 to o.children.count do (
					SetAnimation o.children[i] frameTime
				)
			)
		)
	)
	
	fn ValidXSIFile fileName = (
		result = openFile fileName
		if result == undefined then (
			return false
		)
		else ( --read Header
			header = readLine result
			parsedHeader = filterString header " "
			if (parsedHeader[1] == "xsi") and (parsedHeader[2]=="0101txt" or parsedHeader[2]=="0103txt") then (
				close result
				return true
			)
			else (
				return false
			)
		)
	)
	
	
	-- The main function
	-- filters the incomming line from the filestream
	-- searches for keywords and forwards the algorithm to the appropriate function.
	fn main fistr = (
		endOfStream = false
		while endOfStream == false do (
			-- Reads the next line from the filestream.
			lineIn = readLine fistr
			-- Verifies if the end of file is reached.
			endOfStream = eof fistr
			--lineOut[3] is the minor version of incoming dotXSI file
			case lineOut[3] of (
				("1"):(lineIn = filterString lineIn ", ;\t")
				default:(lineIn = filterString lineIn ", ;\t";)
			)
			case lineIn[1] of (
				("xsi"):(
					PrepareToRead lineIn;
					endOfStream = true;
				)
				("SI_Angle"):(Get_SI_Angle())
				("SI_Camera"):(Get_SI_Camera fistr)
				("SI_Light"):(Get_SI_Light fistr)
				("SI_Ambience"):(Get_SI_Ambience())
				("SI_Fog"):(Get_SI_Fog())
				("SI_CoordinateSystem"):(Get_SI_CoordinateSystem fistr)
				("Frame"):(Get_Frame lineIn;)
				("SI_FrameBasePoseMatrix"):(Get_SI_FrameBasePoseMatrix crrObj)
				("SI_EnvelopeList"):(
					Get_SI_EnvelopeList();
					endOfStream = eof fistr;
				)
				("}"):(
					UpdateObject crrObj;
					try (deleteItem objects_arr objects_arr.count)
					catch ()
				)
				("FrameTransformMatrix"):(Get_FrameTransformMatrix crrObj)
				("Mesh"):(Get_Mesh crrObj)
				("AnimationSet"):(
					/*if axisorder_id == 1 do (
						bObjectAnimation = false
					)*/
					Get_AnimationSet inFile;
					if bAnimImport == true do (
						bObjectAnimation = true;			
						endOfStream = eof fistr;
					)
				)
				default:()
			)
			
			if bProgressFlag == true do (
				Main_rll.pbar_ImportProgress.value = ((filePos inFile)/(max_filepos as float))*100
			)
		)
	)
	

	rollout Authorship_rll "About" width:275 height:182 (
		label lab0 "XSI Import Rollout" width:100 offset:[5,7]
		edittext edtxt_Version1 "v." fieldWidth:24 offset:[65,10] text:("  " + import_rollout_version[1])
		label lbl_Dot "." width:10 offset:[-15,-20]
		edittext edtxt_Version2 "" fieldWidth:30 offset:[107,-20] text:(" " + import_rollout_version[2])
		label lab1 "(Beta)" width:100 offset:[74,-20]
		label lab2 "for 3ds Max only" offset:[0,7]
		label lab3 "(not compatible with Gmax)"
		label lab4 "Copyright \xa9 2014, Tsvetan Stoyanov" offset:[0,10] 
		hyperLink lab5_email "papyrseh\x40gmail.com" align:#center color:blue hoverColor:(color 145 28 177) address:"mailto:papyrseh\x40gmail.com"
		button btn_Ok "Ok" width:75 offset:[0,4]
	  
		on btn_Ok pressed do (
			DestroyDialog Authorship_rll
		)
	)
	
	rollout NoXSIFilesFound_rll "Warning" width:300 height:80 (
		label lab0 "To continue with batch processing please select"
		label lab1 "a directory containing valid dotXSI 1.x files!"
		button btn_Ok "Ok" width:100 offset:[0,10]
		
		on btn_Ok pressed do (
			Main_rll.chkbox_BatchJob.checked = false
			DestroyDialog NoXSIFilesFound_rll
		)
	)
	
	rollout Main_rll "XSI Import Rollout" width:250 height:620 (
		group "About"
		(
			edittext edtxt_Version1 "v." fieldWidth:24 text:("  " + import_rollout_version[1] ) offset:[0,1] readOnly:false
			label lbl_Dot "." width:10 offset:[-66,-20]
			edittext edtxt_Version2 "" fieldWidth:30 text:(" "+ import_rollout_version[2] ) offset:[48,-20] readOnly:false
			button btn_Authorship "Authorship..." width:90 offset:[61,-24]
		)
		group "File Selection"
		(
			edittext edtxt_Directory "File:" width:220 offset:[0,2] readOnly:true
			button btn_Open "Open" offset:[0,0] width:75
		)
		group "Frame Rate"
		(
			radiobuttons rdo_FrameRate labels:#("NTSC","PAL","Film","Custom") default:1 columns:3 align:#left width:200
			spinner spn_FPS "" range:[1, 360, 30] type:#integer offset:[-112,-20] fieldWidth:40 enabled:false
		)
		group "Game Presets"
		(
			dropdownlist ddl_SelectPresets "Select:" items:#("BattleZone 2 (Activision)","Star Trek: Voyager- Elite Force (Activision, Raven)")
		)
		group "Root Orientation"
		(
			checkbox chkbox_PrerotateRoot "PrerotateX root " enabled:false checked:false
			spinner spn_PrerotateRoot range:[0,180,0] enabled:false offset:[-70,-21] fieldWidth:40
			label lbl_RotateRoot "Rotate the Root (consecutive) with:" offset:[-30, 0]
			spinner spn_RootRotateX "x " range:[-180, 180, 0] offset:[-160, 0] fieldWidth:40
			spinner spn_RootRotateY "y " range:[-180, 180, 0] offset:[-95, -21] fieldWidth:40
			spinner spn_RootRotateZ "z " range:[-180, 180, 0] offset:[-30, -21] fieldWidth:40
		)
		group "Import Options"
		(
			checkbox chkbox_MeshNormals "Normals" offset:[0,0] checked:bNormals
			checkbox chkbox_MeshMaterial "Materials" offset:[0,0] checked:bMeshMaterial
			checkbox chkbox_MeshTextureCoords "TextureCoords" offset:[0,0] checked:bMeshTextureCoords
			checkbox chkbox_ObjAnimation "Object Animation" offset:[0,0] checked:true
			checkbox chkbox_Skin "Skin envelopes" offset:[0,0] checked:true
			checkbox chkbox_DelPrefix "Delete prefix from objects:" offset:[0,0] checked:false
			edittext edtxt_Prefix "" fieldWidth:60 offset:[142,-20] text:""
			checkbox chkbox_BatchJob "Batch Processing" checked:false
		)
		
		button btn_Import "Import" offset:[0,9] width:75 enabled:false
		button btn_DeleteAllObjects "Delete scene objects" offset:[0,2] width:115
		progressbar pbar_ImportProgress color:green offset:[0,2]
		progressbar pbar_BatchProgress offset:[0,0] enabled:false
		
		on Main_rll open do (
			if heapSize < (7680000 * 2) do (
				heapSize *= 2  --double heapSize
			)
		)
		
		on btn_Open pressed do (
			odirectory = getopenfilename caption:"Open dotXSI 1.x File" filename:"" types:"Softimage (*.xsi)|*.xsi"
			if odirectory != undefined do (
				directory = ValidateDirectoryName odirectory
				edtxt_Directory.text = directory
				print directory
				inFile = openFile directory
				main inFile
				max_filepos = filepos inFile
				seek inFile lineOut[5]
				btn_Import.enabled = true
			)
		)
		
		on ddl_SelectPresets selected i do (
			case i of (
				(1):(
					axisorder_id = 11;
					chkbox_PrerotateRoot.checked = false;
					chkbox_PrerotateRoot.enabled = false;
					spn_PrerotateRoot.enabled = false
					chkbox_DelPrefix.checked = false
					edtxt_Prefix.text = ""
				)
				(2):(
					axisorder_id = 1;
					chkbox_PrerotateRoot.enabled = true;
					spn_PrerotateRoot.enabled = true
					chkbox_DelPrefix.checked = true
					edtxt_Prefix.text = "frm-"
				)
			)
		)
		
		on rdo_FrameRate changed state do (
			case rdo_FrameRate.state of
			(
				1: ( --"NTSC"
					spn_FPS.enabled = false 
					frameRate = 30
					spn_FPS.value = frameRate

				)
				2: ( --"PAL"
					spn_FPS.enabled = false
					frameRate = 25
					spn_FPS.value = frameRate

				)
				3: ( --"Film"
					spn_FPS.enabled = false
					frameRate = 24
					spn_FPS.value = frameRate

				)
				4: ( --"Custom"
					spn_FPS.enabled = true
					frameRate = xsiFPS
					spn_FPS.value = frameRate

				)
			)
		)
		
		on spn_FPS changed val do (
			--set custom FPS value
			frameRate = spn_FPS.value

		)
		
		on spn_FPS buttonup do (
			spn_FPS.value = frameRate

		)
		
		on spn_FPS entered do (
			--set custom FPS value
			frameRate = spn_FPS.value
			spn_FPS.value = frameRate

			if spn_FPS.value == 30 then (
				rdo_FrameRate.state = 1 --NTSC
				spn_FPS.enabled = false

			)
			else if spn_FPS.value == 25 then (
				rdo_FrameRate.state = 2 --PAL
				spn_FPS.enabled = false

			)
			else if spn_FPS.value == 24 then (
				rdo_FrameRate.state = 3 --Film
				spn_FPS.enabled = false

			)
			else()
		)
		
		on chkbox_PrerotateRoot changed theState do (
			bPrerotateRoot = theState;
		)
		
		on btn_Authorship pressed do (
			CreateDialog Authorship_rll modal:true
		)
		
		on chkbox_MeshNormals changed theState do (
			bNormals = theState
		)
		
		on chkbox_MeshMaterial changed theState do (
			bMeshMaterial = theState
		)
		
		on chkbox_MeshTextureCoords changed theState do (
			bMeshTextureCoords = theState
		)
		
		on chkbox_ObjAnimation changed theState do (
			bObjectAnimation = theState
			bAnimImport = theState
		)
		
		on chkbox_Skin changed theState do (
			bSkinEnvelope = theState
		)

		on chkbox_DelPrefix changed theState do (
		  --print bDeletePrefix
		  bDeletePrefix = theState
		)
		
		on chkbox_BatchJob changed theState do (
			if chkbox_BatchJob.checked then (
				pbar_BatchProgress.enabled = true
				batchDirectory = getSavePath caption:"Get Batch Directory"
				--get list of dotXSI files
				tempList = #()
				if batchDirectory != undefined then (
					tempList = getFiles ( batchDirectory + "\\*.xsi" )
					--validate filelist
					if tempList.count > 0 then (
						for i = 1 to tempList.count do (
							--print tempList[i]
							if (ValidXSIFile tempList[i]) do (
								InsertItem tempList[i] xsiFileList (xsiFileList.count + 1)
							)
						)
						if xsiFileList.count > 0 then ( 
							--for i = 1 to xsiFileList.count do ( print xsiFileList[i] )
							edtxt_Directory.text = xsiFileList[1]
							btn_Import.enabled = true
							btn_Open.enabled = false
						)
						else (
							createDialog NoXSIFilesFound_rll
						)
					)
					else (
						createDialog NoXSIFilesFound_rll
					)
				)
				else (
					createDialog NoXSIFilesFound_rll
				)
			)
			else (
				pbar_BatchProgress.enabled = false
				xsiFileList = #()
				edtxt_Directory.text = ""
				btn_Import.enabled = false
				btn_Open.enabled = true
			)
		)
		
		on btn_Import pressed do (
			if chkbox_BatchJob.checked then (
				pbar_BatchProgress.value = 0
				for i = 1 to xsiFileList.count do (
					resetMaxFile #noPrompt
					frameRate = spn_FPS.value
					animationRange = interval 0f 100f
					sliderTime = 0
					grandpappy.count = 0
					pbar_ImportProgress.value = 0
					bProgressFlag = true
					inFile = openFile xsiFileList[i]
					seek inFile #eof
					max_filepos = filepos inFile
					seek inFile 0
					header = readLine inFile
					header = filterString header " "
					PrepareToRead header
					seek inFile lineOut[5]
					main inFile
					bProgressFlag = false
					if axisorder_id == 1 do (
						if bPrerotateRoot == true do (
							in coordsys #local rotate (grandpappy[1]) (eulerangles (spn_PrerotateRoot.value) 0 0)
						)
						hierarchyRoot = grandpappy[1]
						if bObjectAnimation == true do (
							if numFrames != undefined do (
								-- set animation start to startFrame (hides Basepose on frame 0)
								animationRange = interval startFrame (numFrames as integer)
								for k = 1 to (numFrames as integer) do (
									SetAnimation hierarchyRoot k
								)
							)
						)
						grandpappy.count = 0
						max select none
					)
					
					if ErrorLog != "" then (
						rollout error_roll "Errors" width:200 height:200 
						(
							edittext error_edit "" fieldWidth:190 height:180
							
							on error_roll open do (
								error_edit.text = ErrorLog
							)
						)
						createDialog error_roll
					)
					else (
						format "% ...import successful!\n" (inFile as string)
					)
						
					if bDeletePrefix == true do (
						if edtxt_Prefix.text != "" do (
							for o in $objects do (
								objName = o.Name
								while (substring objName 1 edtxt_Prefix.text.count) == edtxt_Prefix.text do (
									objName = substring objName (edtxt_Prefix.text.count+1) (objName.count-edtxt_Prefix.text.count)
								)
								o.Name = objName
							)
						)
					)
					
					ErrorLog = ""
					
					--save imported file name for 3ds Max file
					tempFileName = xsiFileList[i]
					
					--update batch progress bar
					percentDone = ( i /(xsiFileList.count as float) ) * 100
					Main_rll.pbar_BatchProgress.value = percentDone
					redrawViews()

					-- close the current file
					close inFile
					
					--change extension and save file
					tempFileName = substring tempFileName 1 (tempFileName.count - 3)
					tempFileName = append tempFileName "max"
					saveMaxFile tempFileName clearNeedSaveFlag:true useNewFile:false
				)
				
				--reset batch processing or close import dialog
				if queryBox "Batch import complete.\nDo you want to continue importing?" title:"Batch Processing..." then (
					--clear file list
					xsiFileList = #()
					Main_rll.pbar_BatchProgress.value = 0
					Main_rll.pbar_ImportProgress.value = 0
					Main_rll.chkbox_BatchJob.checked = false
					Main_rll.edtxt_Directory.text = ""
					Main_rll.btn_Open.enabled = true
				)
				else (
					DestroyDialog Main_rll
				)
			)
			else (
				animationRange = interval 0 100
				sliderTime = 0
				grandpappy.count = 0
				--print (axisorder[axisorder_id]) #nomap
				pbar_ImportProgress.value = 0
				bProgressFlag = true
				main inFile
				bProgressFlag = false
				--print inFile
				seek inFile lineOut[5]
				if axisorder_id == 1 do (
					if bPrerotateRoot == true do (
						in coordsys #local rotate (grandpappy[1]) (eulerangles (spn_PrerotateRoot.value) 0 0)
					)
					hierarchyRoot = grandpappy[1]
					if bObjectAnimation == true do (
						if numFrames != undefined do (
							animationRange = interval 0 (numFrames as integer)
							for k = 1 to (numFrames as integer) do (
								SetAnimation hierarchyRoot k
							)
							-- set animation to startFrame (hides basepose on frame 0)
							animationRange = interval startFrame (numFrames as integer)	
						)
					)
					grandpappy.count = 0
					max select none
				)
				
				if ErrorLog != "" then (
					rollout error_roll "Errors" width:200 height:200 
					(
						edittext error_edit "" fieldWidth:190 height:180
						
						on error_roll open do (
							error_edit.text = ErrorLog
						)
					)
					createDialog error_roll
				)
				else (
					format "% ...import successful!\n" (inFile as string)
				)
					
				if bDeletePrefix == true do (
					if edtxt_Prefix.text != "" do (
						for o in $objects do (
							objName = o.Name
							while (substring objName 1 edtxt_Prefix.text.count) == edtxt_Prefix.text do (
								objName = substring objName (edtxt_Prefix.text.count+1) (objName.count-edtxt_Prefix.text.count)
							)
							o.Name = objName
						)
					)
				)
				
				ErrorLog = ""
				seek inFile lineOut[5]
			)
		)
		
		on btn_DeleteAllObjects pressed do with undo "Delete Scene Objects" on (
			obj = #()
			for o in objects do (
				insertitem o obj (obj.count + 1)
			)
			for o in obj do (
				try (delete o) catch ()
			)
		)

	)
	CreateDialog Main_rll
	
  )